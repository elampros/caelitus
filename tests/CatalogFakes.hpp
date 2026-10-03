#pragma once

/// @file
/// In-memory catalog repositories, a transaction manager with real rollback
/// (snapshot/restore) and a recording publisher, for service and API tests.
///
/// All fakes share one fakes::Store, so a test can build real services on them
/// and then look at the store directly:
///
/// @code
/// fakes::Store store;
/// catalog::CategoryService categories(std::make_shared<fakes::FakeCategories>(store));
/// categories.create("Science Fiction");
/// CHECK_EQ(store.categories.size(), 1u);
/// @endcode
///
/// The fakes enforce the same uniqueness and version rules as the SQL
/// repositories, throwing the same domain errors, so service tests exercise the
/// real error paths.
/// @ingroup tests

#include "TestHarness.hpp"

#include "caelitus/catalog/domain/Reactions.hpp"
#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/core/DomainErrors.hpp"
#include "caelitus/db/DbErrors.hpp"
#include "caelitus/db/ITransactionManager.hpp"
#include "caelitus/mqtt/IMqttClient.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

/// In-memory stand-ins for the catalog's repositories, transactions and MQTT publisher.
namespace fakes {

using namespace caelitus;
using namespace caelitus::catalog;

// ---- In-memory storage -------------------------------------------------------

/// The "database" of the fakes: plain maps by id. Copyable, which is how
/// FakeTx rolls back.
struct Store {
    std::map<std::int64_t, Category> categories;                                     ///< By id.
    std::map<std::int64_t, Author> authors;                                          ///< By id.
    std::map<std::int64_t, Tag> tags;                                                ///< By id.
    std::map<std::int64_t, Book> books;                                              ///< By id.
    std::map<std::int64_t, Review> reviews;                                          ///< By id.
    std::map<std::pair<std::int64_t, std::int64_t>, ReactionCounts> dailyReactions;  ///< (book, day number) -> counts.
    std::map<std::int64_t, ReactionCounts> reactionTotals;                           ///< Book -> all-time counts.
    std::int64_t nextId = 1;  ///< Next id for any entity (ids are unique across tables).

    /// True if some book lists author `a` (the "author has books" rule).
    bool bookUsesAuthor(AuthorId a) const {
        for (const auto& [id, b] : books)
            if (std::find(b.authorIds.begin(), b.authorIds.end(), a) != b.authorIds.end()) return true;
        return false;
    }
};

/// Runs work directly; restores the store if it throws (a real rollback).
/// Nested transactions join the outer one, as with the real manager.
class FakeTx final : public db::ITransactionManager {
public:
    /// @param s  The store to snapshot and restore.
    explicit FakeTx(Store& s) : store_(s) {}
    int commits = 0;                              ///< Outermost transactions that succeeded.
    int rollbacks = 0;                            ///< Outermost transactions that threw and were undone.
    std::vector<db::TransactionOptions> options;  ///< The options of every outermost transaction.

protected:
    void run(const db::TransactionOptions& opts, const std::function<void()>& work) override {
        if (depth_ > 0) return work();
        std::lock_guard<std::recursive_mutex> lock(mutex_);  // one transaction at a time
        options.push_back(opts);
        Store snapshot = store_;
        ++depth_;
        try {
            work();
            --depth_;
            ++commits;
        } catch (...) {
            --depth_;
            store_ = snapshot;
            ++rollbacks;
            throw;
        }
    }

private:
    Store& store_;
    static thread_local int depth_;
    std::recursive_mutex mutex_;
};

inline thread_local int FakeTx::depth_ = 0;

/// Categories; names and slugs are unique (`name_taken`, `slug_taken`).
class FakeCategories final : public ICategoryRepository {
public:
    /// @param s  Where the data lives.
    explicit FakeCategories(Store& s) : s_(s) {}
    CategoryId insert(const Category& c) override {
        checkUnique(c);
        Category stored = c;
        stored.id = CategoryId(s_.nextId++);
        s_.categories[stored.id.value] = stored;
        return stored.id;
    }
    std::optional<Category> findById(CategoryId id) override {
        auto it = s_.categories.find(id.value);
        if (it == s_.categories.end()) return std::nullopt;
        return it->second;
    }
    std::vector<Category> listAll() override {
        std::vector<Category> out;
        for (auto& [id, c] : s_.categories) out.push_back(c);
        return out;
    }
    bool update(const Category& c) override {
        if (!s_.categories.count(c.id.value)) return false;
        checkUnique(c);
        s_.categories[c.id.value] = c;
        return true;
    }
    bool remove(CategoryId id) override {
        for (auto& [bid, b] : s_.books)
            if (b.categoryId == id) throw ConflictError("category_in_use", "in use");
        return s_.categories.erase(id.value) > 0;
    }

private:
    void checkUnique(const Category& c) {
        for (auto& [id, o] : s_.categories) {
            if (o.id == c.id) continue;
            if (o.name == c.name) throw ConflictError("name_taken", "dup name");
            if (o.slug == c.slug) throw ConflictError("slug_taken", "dup slug");
        }
    }
    Store& s_;
};

/// Authors, with optimistic locking on `version`.
class FakeAuthors final : public IAuthorRepository {
public:
    /// @param s  Where the data lives.
    explicit FakeAuthors(Store& s) : s_(s) {}
    AuthorId insert(const Author& a) override {
        Author stored = a;
        stored.id = AuthorId(s_.nextId++);
        stored.version = 1;
        s_.authors[stored.id.value] = stored;
        return stored.id;
    }
    std::optional<Author> findById(AuthorId id) override {
        auto it = s_.authors.find(id.value);
        if (it == s_.authors.end()) return std::nullopt;
        return it->second;
    }
    std::vector<Author> findByIds(const std::vector<AuthorId>& ids) override {
        std::vector<Author> out;
        for (auto id : ids)
            if (auto a = findById(id)) out.push_back(*a);
        return out;
    }
    Paged<Author> search(const std::optional<std::string>& name, const Page& page) override {
        Paged<Author> out;
        out.page = page;
        for (auto& [id, a] : s_.authors)
            if (!name || a.name.find(*name) != std::string::npos) out.items.push_back(a);
        out.total = static_cast<std::int64_t>(out.items.size());
        return out;
    }
    UpdateResult update(const Author& a, int expectedVersion) override {
        auto it = s_.authors.find(a.id.value);
        if (it == s_.authors.end()) return UpdateResult::NotFound;
        if (it->second.version != expectedVersion) return UpdateResult::VersionConflict;
        Author stored = a;
        stored.createdAt = it->second.createdAt;
        stored.version = expectedVersion + 1;
        it->second = stored;
        return UpdateResult::Updated;
    }
    bool remove(AuthorId id) override {
        if (s_.bookUsesAuthor(id)) throw ConflictError("author_has_books", "has books");
        return s_.authors.erase(id.value) > 0;
    }

private:
    Store& s_;
};

/// Tags, created on first use.
class FakeTags final : public ITagRepository {
public:
    /// @param s  Where the data lives.
    explicit FakeTags(Store& s) : s_(s) {}
    std::vector<Tag> findOrCreate(const std::vector<std::string>& names) override {
        std::vector<Tag> out;
        for (const auto& n : names) {
            auto it = std::find_if(s_.tags.begin(), s_.tags.end(), [&](auto& kv) { return kv.second.name == n; });
            if (it == s_.tags.end()) {
                Tag t{TagId(s_.nextId++), n};
                s_.tags[t.id.value] = t;
                out.push_back(t);
            } else {
                out.push_back(it->second);
            }
        }
        return out;
    }
    std::vector<TagUsage> listUsed() override { return {}; }

private:
    Store& s_;
};

/// Books: search filters, ISBN uniqueness, optimistic locking, rating updates.
class FakeBooks final : public IBookRepository {
public:
    /// @param s  Where the data lives.
    explicit FakeBooks(Store& s) : s_(s) {}
    std::optional<BookQuery> lastQuery;  ///< The last search(), to check how services build queries.

    BookId insert(const Book& b) override {
        checkIsbn(b);
        Book stored = b;
        stored.id = BookId(s_.nextId++);
        stored.version = 1;
        s_.books[stored.id.value] = stored;
        return stored.id;
    }
    bool exists(BookId id) override { return s_.books.count(id.value) > 0; }
    std::optional<Book> findById(BookId id) override {
        auto it = s_.books.find(id.value);
        if (it == s_.books.end()) return std::nullopt;
        return it->second;
    }
    std::optional<BookDetails> details(BookId id) override {
        auto b = findById(id);
        if (!b) return std::nullopt;
        BookDetails d;
        d.id = b->id;
        d.title = b->title;
        d.publishedOn = b->publishedOn;
        d.language = b->language;
        d.category = s_.categories.at(b->categoryId.value);
        for (auto a : b->authorIds) d.authors.push_back({a, s_.authors.at(a.value).name});
        for (auto t : b->tagIds) d.tags.push_back(s_.tags.at(t.value).name);
        std::sort(d.tags.begin(), d.tags.end());
        d.ratingCount = b->ratingCount;
        d.ratingAverage = averageRating(b->ratingCount, b->ratingSum);
        d.reactionsEnabled = b->reactionsEnabled;
        if (s_.reactionTotals.count(id.value)) {
            d.likes = s_.reactionTotals.at(id.value).likes;
            d.dislikes = s_.reactionTotals.at(id.value).dislikes;
        }
        d.isbn = b->isbn;
        d.description = b->description;
        d.pageCount = b->pageCount;
        d.version = b->version;
        d.createdAt = b->createdAt;
        d.updatedAt = b->updatedAt;
        return d;
    }
    Paged<BookSummary> search(const BookQuery& q) override {
        lastQuery = q;
        Paged<BookSummary> out;
        out.page = q.page;
        for (auto& [id, b] : s_.books) out.items.push_back(*details(b.id));
        out.total = static_cast<std::int64_t>(out.items.size());
        return out;
    }
    std::vector<BookBrief> listBriefs() override {
        std::vector<BookBrief> out;
        for (auto& [id, b] : s_.books) out.push_back({b.id, b.title, b.reactionsEnabled});
        return out;
    }
    std::optional<BookBrief> findBrief(BookId id) override {
        auto b = findById(id);
        if (!b) return std::nullopt;
        return BookBrief{b->id, b->title, b->reactionsEnabled};
    }
    bool setReactionsEnabled(BookId id, bool enabled) override {
        auto it = s_.books.find(id.value);
        if (it == s_.books.end()) return false;
        it->second.reactionsEnabled = enabled;
        return true;
    }
    UpdateResult update(const Book& b, int expectedVersion) override {
        auto it = s_.books.find(b.id.value);
        if (it == s_.books.end()) return UpdateResult::NotFound;
        if (it->second.version != expectedVersion) return UpdateResult::VersionConflict;
        checkIsbn(b);
        Book stored = b;
        stored.createdAt = it->second.createdAt;
        stored.ratingCount = it->second.ratingCount;
        stored.ratingSum = it->second.ratingSum;
        stored.reactionsEnabled = it->second.reactionsEnabled;  // not changed by update
        stored.version = expectedVersion + 1;
        it->second = stored;
        return UpdateResult::Updated;
    }
    bool remove(BookId id) override {
        s_.reactionTotals.erase(id.value);
        for (auto it = s_.dailyReactions.begin(); it != s_.dailyReactions.end();)
            it = it->first.first == id.value ? s_.dailyReactions.erase(it) : std::next(it);
        for (auto it = s_.reviews.begin(); it != s_.reviews.end();)
            it = it->second.bookId == id ? s_.reviews.erase(it) : std::next(it);
        return s_.books.erase(id.value) > 0;
    }
    bool adjustRating(BookId id, int countDelta, int sumDelta) override {
        auto it = s_.books.find(id.value);
        if (it == s_.books.end()) return false;
        it->second.ratingCount += countDelta;
        it->second.ratingSum += sumDelta;
        return true;
    }

private:
    void checkIsbn(const Book& b) {
        if (!b.isbn) return;
        for (auto& [id, o] : s_.books)
            if (o.id != b.id && o.isbn == b.isbn) throw ConflictError("isbn_taken", "dup isbn");
    }
    Store& s_;
};

/// Reviews of books.
class FakeReviews final : public IReviewRepository {
public:
    /// @param s  Where the data lives.
    explicit FakeReviews(Store& s) : s_(s) {}
    ReviewId insert(const Review& r) override {
        Review stored = r;
        stored.id = ReviewId(s_.nextId++);
        s_.reviews[stored.id.value] = stored;
        return stored.id;
    }
    std::optional<Review> findById(ReviewId id) override {
        auto it = s_.reviews.find(id.value);
        if (it == s_.reviews.end()) return std::nullopt;
        return it->second;
    }
    std::optional<Review> lockById(ReviewId id) override { return findById(id); }
    Paged<Review> listByBook(BookId book, const Page& page) override {
        Paged<Review> out;
        out.page = page;
        for (auto& [id, r] : s_.reviews)
            if (r.bookId == book) out.items.push_back(r);
        out.total = static_cast<std::int64_t>(out.items.size());
        return out;
    }
    bool update(const Review& r) override {
        if (!s_.reviews.count(r.id.value)) return false;
        s_.reviews[r.id.value] = r;
        return true;
    }
    bool remove(ReviewId id) override { return s_.reviews.erase(id.value) > 0; }

private:
    Store& s_;
};

/// Records every published message instead of sending it.
class FakePublisher final : public mqtt::IMqttPublisher {
public:
    std::vector<std::pair<std::string, std::string>> sent;  ///< (topic, payload), in publish order.

protected:
    bool doPublish(std::string_view topic, std::string_view payload, const mqtt::PublishOptions&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sent.emplace_back(std::string(topic), std::string(payload));
        return true;
    }

private:
    std::mutex mutex_;
};

/// Per-day like/dislike counts, with a switch to simulate a failing database.
class FakeReactions final : public IReactionRepository {
public:
    /// @param s  Where the counts live.
    explicit FakeReactions(Store& s) : s_(s) {}
    std::atomic<bool> failNext{false};  ///< Make the next add() throw, as if the database were down.
    std::atomic<int> addCalls{0};       ///< How many times add() was called (flush checks).

    std::int64_t add(const std::vector<DailyReactions>& deltas) override {
        ++addCalls;
        if (failNext.exchange(false)) throw db::ConnectionError("database down");
        std::int64_t skipped = 0;
        for (const auto& d : deltas) {
            if (!s_.books.count(d.book.value)) {
                skipped += d.likes + d.dislikes;
                continue;
            }
            auto& day = s_.dailyReactions[{d.book.value, d.day.toDays()}];
            day.likes += d.likes;
            day.dislikes += d.dislikes;
            auto& total = s_.reactionTotals[d.book.value];
            total.likes += d.likes;
            total.dislikes += d.dislikes;
        }
        return skipped;
    }

    std::vector<ReactionCounts> counts(BookId book, const std::vector<std::optional<DateRange>>& ranges) override {
        std::vector<ReactionCounts> out;
        for (const auto& r : ranges) out.push_back(sum(book.value, r));
        return out;
    }

    std::vector<RankedBook> top(const std::optional<DateRange>& range, ReactionOrder order, int limit) override {
        std::vector<RankedBook> out;
        for (const auto& [id, b] : s_.books) {
            const ReactionCounts c = sum(id, range);
            const bool include = order == ReactionOrder::MostLiked ? c.likes > 0 : c.dislikes > 0;
            if (include) out.push_back({b.id, b.title, c});
        }
        std::sort(out.begin(), out.end(), [order](const RankedBook& a, const RankedBook& b) {
            return order == ReactionOrder::MostLiked ? a.counts.score() > b.counts.score()
                                                     : a.counts.score() < b.counts.score();
        });
        if (out.size() > static_cast<std::size_t>(limit)) out.resize(static_cast<std::size_t>(limit));
        return out;
    }

    std::int64_t deleteBefore(const Date& day) override {
        std::int64_t n = 0;
        for (auto it = s_.dailyReactions.begin(); it != s_.dailyReactions.end();)
            if (it->first.second < day.toDays()) {
                it = s_.dailyReactions.erase(it);
                ++n;
            } else {
                ++it;
            }
        return n;
    }

private:
    ReactionCounts sum(std::int64_t book, const std::optional<DateRange>& range) const {
        if (!range) return s_.reactionTotals.count(book) ? s_.reactionTotals.at(book) : ReactionCounts{};
        ReactionCounts c;
        for (const auto& [key, counts] : s_.dailyReactions)
            if (key.first == book && key.second >= range->from.toDays() && key.second <= range->to.toDays()) {
                c.likes += counts.likes;
                c.dislikes += counts.dislikes;
            }
        return c;
    }
    Store& s_;
};

}  // namespace fakes
