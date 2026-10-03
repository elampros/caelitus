/// @file
/// The book repository in SQL: search with every filter and sort order,
/// authors and tags per book, optimistic locking, row locks for rating updates.
/// @ingroup catalog_mariadb

#include "caelitus/catalog/mariadb/MariaDbRepositories.hpp"

#include "caelitus/core/DomainErrors.hpp"
#include "caelitus/db/DbErrors.hpp"
#include "catalog/mariadb/SqlFilter.hpp"

#include <unordered_map>

namespace caelitus::catalog::mariadb {

using db::Row;

namespace {

constexpr const char* kBookColumns =
    "id, title, isbn, description, published_on, language, page_count, category_id, rating_count, rating_sum,"
    " reactions_enabled, version, created_at, updated_at";

// Summary columns, prefixed for the books b JOIN categories c queries.
constexpr const char* kSummaryColumns =
    "b.id, b.title, b.published_on, b.language, b.rating_count, b.rating_sum, b.likes, b.dislikes,"
    " b.reactions_enabled,"
    " c.id AS category_id, c.name AS category_name, c.slug AS category_slug";

void fillSummary(BookSummary& s, const Row& r) {
    s.id = BookId(r.get<std::int64_t>("id"));
    s.title = r.get<std::string>("title");
    s.publishedOn = r.get<Date>("published_on");
    s.language = r.get<std::string>("language");
    s.category = {CategoryId(r.get<std::int64_t>("category_id")), r.get<std::string>("category_name"),
                  r.get<std::string>("category_slug")};
    s.ratingCount = r.get<int>("rating_count");
    s.ratingAverage = averageRating(s.ratingCount, r.get<int>("rating_sum"));
    s.likes = r.get<std::int64_t>("likes");
    s.dislikes = r.get<std::int64_t>("dislikes");
    s.reactionsEnabled = r.get<bool>("reactions_enabled");
}

// Whitelisted ORDER BY clauses; b.id makes the order total so pages are stable.
const char* orderBy(BookSort sort) {
    switch (sort) {
        case BookSort::PublishedAsc: return " ORDER BY b.published_on ASC, b.id ASC";
        case BookSort::TitleAsc: return " ORDER BY b.title ASC, b.id ASC";
        case BookSort::RatingDesc:
            return " ORDER BY (b.rating_count = 0) ASC, b.rating_sum / NULLIF(b.rating_count, 0) DESC,"
                   " b.rating_count DESC, b.id DESC";
        case BookSort::CreatedDesc: return " ORDER BY b.created_at DESC, b.id DESC";
        case BookSort::PublishedDesc: break;
    }
    return " ORDER BY b.published_on DESC, b.id DESC";
}

[[noreturn]] void rethrowIsbnTaken() { throw ConflictError("isbn_taken", "A book with this ISBN already exists"); }

[[noreturn]] void rethrowStaleReference() {
    throw ConflictError("stale_reference", "A referenced category, author or tag no longer exists");
}

}  // namespace

BookId MariaDbBookRepository::insert(const Book& b) {
    BookId id;
    try {
        auto r = sql_->insert(
            "INSERT INTO books (title, isbn, description, published_on, language, page_count, category_id,"
            " rating_count, rating_sum, reactions_enabled, version, created_at, updated_at)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, 0, 0, ?, 1, ?, ?)",
            {b.title, b.isbn, b.description, b.publishedOn, b.language, b.pageCount, b.categoryId.value,
             b.reactionsEnabled, b.createdAt, b.updatedAt});
        id = BookId(static_cast<std::int64_t>(r.lastInsertId));
    } catch (const db::DuplicateKeyError&) {
        rethrowIsbnTaken();
    } catch (const db::ForeignKeyError&) {
        rethrowStaleReference();
    }
    writeLinks(id, b, false);
    return id;
}

void MariaDbBookRepository::writeLinks(BookId id, const Book& b, bool replace) {
    try {
        if (replace) {
            sql_->execute("DELETE FROM book_authors WHERE book_id = ?", {id.value});
            sql_->execute("DELETE FROM book_tags WHERE book_id = ?", {id.value});
        }
        if (!b.authorIds.empty()) {
            std::string values;
            db::Params params;
            for (std::size_t i = 0; i < b.authorIds.size(); ++i) {
                values += (i == 0 ? "(?, ?, ?)" : ", (?, ?, ?)");
                params.insert(params.end(), {id.value, b.authorIds[i].value, static_cast<int>(i + 1)});
            }
            sql_->execute("INSERT INTO book_authors (book_id, author_id, position) VALUES " + values, params);
        }
        if (!b.tagIds.empty()) {
            std::string values;
            db::Params params;
            for (std::size_t i = 0; i < b.tagIds.size(); ++i) {
                values += (i == 0 ? "(?, ?)" : ", (?, ?)");
                params.insert(params.end(), {id.value, b.tagIds[i].value});
            }
            sql_->execute("INSERT INTO book_tags (book_id, tag_id) VALUES " + values, params);
        }
    } catch (const db::ForeignKeyError&) {
        rethrowStaleReference();
    }
}

std::optional<Book> MariaDbBookRepository::findById(BookId id) {
    auto row = sql_->queryOne(std::string("SELECT ") + kBookColumns + " FROM books WHERE id = ?", {id.value});
    if (!row) return std::nullopt;
    const Row& r = *row;

    Book b;
    b.id = id;
    b.title = r.get<std::string>("title");
    b.isbn = r.getOptional<std::string>("isbn");
    b.description = r.getOptional<std::string>("description");
    b.publishedOn = r.get<Date>("published_on");
    b.language = r.get<std::string>("language");
    b.pageCount = r.getOptional<int>("page_count");
    b.categoryId = CategoryId(r.get<std::int64_t>("category_id"));
    b.ratingCount = r.get<int>("rating_count");
    b.ratingSum = r.get<int>("rating_sum");
    b.reactionsEnabled = r.get<bool>("reactions_enabled");
    b.version = r.get<int>("version");
    b.createdAt = r.get<Timestamp>("created_at");
    b.updatedAt = r.get<Timestamp>("updated_at");
    b.authorIds = sql_->queryList("SELECT author_id FROM book_authors WHERE book_id = ? ORDER BY position", {id.value},
                                  [](const Row& x) { return AuthorId(x.get<std::int64_t>(0)); });
    b.tagIds = sql_->queryList("SELECT tag_id FROM book_tags WHERE book_id = ? ORDER BY tag_id", {id.value},
                               [](const Row& x) { return TagId(x.get<std::int64_t>(0)); });
    return b;
}

std::optional<BookDetails> MariaDbBookRepository::details(BookId id) {
    auto row = sql_->queryOne(std::string("SELECT ") + kSummaryColumns +
                                  ", b.isbn, b.description, b.page_count, b.version, b.created_at, b.updated_at"
                                  " FROM books b JOIN categories c ON c.id = b.category_id WHERE b.id = ?",
                              {id.value});
    if (!row) return std::nullopt;

    BookDetails d;
    fillSummary(d, *row);
    d.isbn = row->getOptional<std::string>("isbn");
    d.description = row->getOptional<std::string>("description");
    d.pageCount = row->getOptional<int>("page_count");
    d.version = row->get<int>("version");
    d.createdAt = row->get<Timestamp>("created_at");
    d.updatedAt = row->get<Timestamp>("updated_at");

    std::vector<BookSummary*> one{&d};
    loadAuthorsAndTags(one);
    return d;
}

Paged<BookSummary> MariaDbBookRepository::search(const BookQuery& q) {
    SqlFilter filter;
    if (q.category) filter.add("b.category_id = ?", {q.category->value});
    if (q.author)
        filter.add("EXISTS (SELECT 1 FROM book_authors ba WHERE ba.book_id = b.id AND ba.author_id = ?)",
                   {q.author->value});
    if (!q.tags.empty()) {
        db::Params names(q.tags.begin(), q.tags.end());
        const std::string matching =
            " FROM book_tags bt JOIN tags t ON t.id = bt.tag_id"
            " WHERE bt.book_id = b.id AND t.name IN (" +
            placeholders(q.tags.size()) + ")";
        if (q.tagMatch == TagMatch::Any) {
            filter.add("EXISTS (SELECT 1" + matching + ")", names);
        } else {
            names.emplace_back(q.tags.size());
            filter.add("(SELECT COUNT(DISTINCT bt.tag_id)" + matching + ") = ?", names);
        }
    }
    if (q.publishedFrom) filter.add("b.published_on >= ?", {*q.publishedFrom});
    if (q.publishedTo) filter.add("b.published_on <= ?", {*q.publishedTo});
    if (q.titleContains) filter.add("b.title LIKE ?", {likeContains(*q.titleContains)});
    if (q.language) filter.add("b.language = ?", {*q.language});
    // average >= min, without dividing: sum >= min * count
    if (q.minRating) filter.add("b.rating_count > 0 AND b.rating_sum >= ? * b.rating_count", {*q.minRating});

    Paged<BookSummary> out;
    out.page = q.page;
    out.total =
        sql_->queryScalar<std::int64_t>("SELECT COUNT(*) FROM books b" + filter.where(), filter.params()).value_or(0);
    if (out.total == 0 || q.page.offset() >= out.total) return out;

    db::Params params = filter.params();
    params.emplace_back(q.page.size);
    params.emplace_back(q.page.offset());
    out.items = sql_->queryList(std::string("SELECT ") + kSummaryColumns +
                                    " FROM books b JOIN categories c ON c.id = b.category_id" + filter.where() +
                                    orderBy(q.sort) + " LIMIT ? OFFSET ?",
                                params, [](const Row& r) {
                                    BookSummary s;
                                    fillSummary(s, r);
                                    return s;
                                });

    std::vector<BookSummary*> page;
    for (auto& s : out.items) page.push_back(&s);
    loadAuthorsAndTags(page);
    return out;
}

// Two queries for the whole page instead of two per book.
void MariaDbBookRepository::loadAuthorsAndTags(std::vector<BookSummary*>& books) {
    if (books.empty()) return;
    std::unordered_map<std::int64_t, BookSummary*> byId;
    db::Params ids;
    for (auto* b : books) {
        byId.emplace(b->id.value, b);
        ids.emplace_back(b->id.value);
    }
    const std::string in = placeholders(ids.size());

    for (const auto& r : sql_->query("SELECT ba.book_id, a.id, a.name FROM book_authors ba JOIN authors a"
                                     " ON a.id = ba.author_id WHERE ba.book_id IN (" +
                                         in + ") ORDER BY ba.book_id, ba.position",
                                     ids))
        byId.at(r.get<std::int64_t>("book_id"))
            ->authors.push_back({AuthorId(r.get<std::int64_t>("id")), r.get<std::string>("name")});

    for (const auto& r : sql_->query("SELECT bt.book_id, t.name FROM book_tags bt JOIN tags t ON t.id = bt.tag_id"
                                     " WHERE bt.book_id IN (" +
                                         in + ") ORDER BY bt.book_id, t.name",
                                     ids))
        byId.at(r.get<std::int64_t>("book_id"))->tags.push_back(r.get<std::string>("name"));
}

UpdateResult MariaDbBookRepository::update(const Book& b, int expectedVersion) {
    db::ExecResult r;
    try {
        r = sql_->execute(
            "UPDATE books SET title = ?, isbn = ?, description = ?, published_on = ?, language = ?, page_count = ?,"
            " category_id = ?, updated_at = ?, version = version + 1 WHERE id = ? AND version = ?",
            {b.title, b.isbn, b.description, b.publishedOn, b.language, b.pageCount, b.categoryId.value, b.updatedAt,
             b.id.value, expectedVersion});
    } catch (const db::DuplicateKeyError&) {
        rethrowIsbnTaken();
    } catch (const db::ForeignKeyError&) {
        rethrowStaleReference();
    }
    if (r.affectedRows == 0) return exists(b.id) ? UpdateResult::VersionConflict : UpdateResult::NotFound;
    writeLinks(b.id, b, true);
    return UpdateResult::Updated;
}

std::vector<BookBrief> MariaDbBookRepository::listBriefs() {
    return sql_->queryList("SELECT id, title, reactions_enabled FROM books", {}, [](const Row& r) {
        return BookBrief{BookId(r.get<std::int64_t>("id")), r.get<std::string>("title"),
                         r.get<bool>("reactions_enabled")};
    });
}

std::optional<BookBrief> MariaDbBookRepository::findBrief(BookId id) {
    auto row = sql_->queryOne("SELECT id, title, reactions_enabled FROM books WHERE id = ?", {id.value});
    if (!row) return std::nullopt;
    return BookBrief{id, row->get<std::string>("title"), row->get<bool>("reactions_enabled")};
}

bool MariaDbBookRepository::setReactionsEnabled(BookId id, bool enabled) {
    auto r = sql_->execute("UPDATE books SET reactions_enabled = ? WHERE id = ?", {enabled, id.value});
    return r.affectedRows > 0 || exists(id);  // unchanged value: 0 rows affected
}

bool MariaDbBookRepository::remove(BookId id) {
    return sql_->execute("DELETE FROM books WHERE id = ?", {id.value}).affectedRows > 0;  // links and reviews cascade
}

bool MariaDbBookRepository::adjustRating(BookId id, int countDelta, int sumDelta) {
    if (countDelta == 0 && sumDelta == 0) return exists(id);  // an UPDATE that changes nothing affects 0 rows
    return sql_->execute("UPDATE books SET rating_count = rating_count + ?, rating_sum = rating_sum + ? WHERE id = ?",
                         {countDelta, sumDelta, id.value})
               .affectedRows > 0;
}

bool MariaDbBookRepository::exists(BookId id) {
    return sql_->queryOne("SELECT 1 FROM books WHERE id = ?", {id.value}).has_value();
}

}  // namespace caelitus::catalog::mariadb
