/// @file
/// Unit tests for the catalog rules and services, with in-memory repositories.
/// No database: these check business rules, validation, transactions
/// (rollback on failure) and events.
/// @ingroup tests

#include "CatalogFakes.hpp"
#include "TestHarness.hpp"

#include "caelitus/catalog/domain/Rules.hpp"
#include "caelitus/catalog/service/AuthorService.hpp"
#include "caelitus/catalog/service/BookCache.hpp"
#include "caelitus/catalog/service/BookService.hpp"
#include "caelitus/catalog/service/CategoryService.hpp"
#include "caelitus/catalog/service/ReactionService.hpp"
#include "caelitus/catalog/service/ReviewService.hpp"

#include <algorithm>
#include <map>
#include <thread>

using namespace caelitus;
using namespace caelitus::catalog;

using namespace fakes;

namespace {

// ---- Wiring ------------------------------------------------------------------

const Timestamp kNow = fromParts({Date(2026, 10, 2), 12, 0, 0, 0});

struct Catalog {
    Store store;
    std::shared_ptr<FakeTx> tx = std::make_shared<FakeTx>(store);
    std::shared_ptr<FakeCategories> categoryRepo = std::make_shared<FakeCategories>(store);
    std::shared_ptr<FakeAuthors> authorRepo = std::make_shared<FakeAuthors>(store);
    std::shared_ptr<FakeTags> tagRepo = std::make_shared<FakeTags>(store);
    std::shared_ptr<FakeBooks> bookRepo = std::make_shared<FakeBooks>(store);
    std::shared_ptr<FakeReviews> reviewRepo = std::make_shared<FakeReviews>(store);
    std::shared_ptr<FakePublisher> events = std::make_shared<FakePublisher>();
    std::shared_ptr<FakeReactions> reactionRepo = std::make_shared<FakeReactions>(store);
    Timestamp now = kNow;  // tests move time forward
    Clock clock = [this] { return now; };

    std::shared_ptr<BookCache> bookCache = std::make_shared<BookCache>(bookRepo);
    CategoryService categories{categoryRepo};
    AuthorService authors{authorRepo, tx, clock};
    BookService books{bookRepo, authorRepo, categoryRepo, tagRepo, tx, events, clock, bookCache};
    ReviewService reviews{reviewRepo, bookRepo, tx, events, clock};
    ReactionService reactions{reactionRepo, bookRepo, bookCache, tx, TimeZone::named("Europe/Athens"), 1000, clock};

    // A valid book input with a fresh category and author.
    BookInput validBook() {
        if (!sciFi) sciFi = categories.create("Science Fiction").id;
        if (!asimov) asimov = authors.create({"Isaac Asimov", std::nullopt, Date(1920, 1, 2)}).id;
        BookInput in;
        in.title = "Foundation";
        in.isbn = "0-553-29335-4";
        in.publishedOn = Date(1951, 6, 1);
        in.language = "EN";
        in.pageCount = 255;
        in.category = *sciFi;
        in.authors = {*asimov};
        in.tags = {"Space Opera", "classic", "  space   opera "};
        in.reactionsEnabled = true;
        return in;
    }

    std::optional<CategoryId> sciFi;
    std::optional<AuthorId> asimov;
};

// Expects a DomainError of type E whose field/code contains `what`.
template <typename E, typename F>
void expectError(F&& f, const std::string& what) {
    try {
        f();
    } catch (const E& e) {
        std::string id;
        if constexpr (std::is_same_v<E, ValidationError>) id = e.field();
        else id = e.code();
        if (id != what) throw test::Failure{"expected '" + what + "', got '" + id + "': " + e.what()};
        return;
    } catch (const std::exception& e) {
        throw test::Failure{std::string("wrong exception: ") + e.what()};
    }
    throw test::Failure{"no exception; expected " + what};
}

}  // namespace

// ---- Rules -------------------------------------------------------------------

TEST(isbn_normalization) {
    CHECK_EQ(rules::normalizeIsbn("0-306-40615-2"), "9780306406157");
    CHECK_EQ(rules::normalizeIsbn("978-0-306-40615-7"), "9780306406157");
    CHECK_EQ(rules::normalizeIsbn("0 8044 2957 x"), "9780804429573");
    expectError<ValidationError>([] { rules::normalizeIsbn("0-306-40615-3"); }, "isbn");
    expectError<ValidationError>([] { rules::normalizeIsbn("978-0-306-40615-8"); }, "isbn");
    expectError<ValidationError>([] { rules::normalizeIsbn("12345"); }, "isbn");
    expectError<ValidationError>([] { rules::normalizeIsbn("9770306406157"); }, "isbn");  // not 978/979
}

TEST(slugs) {
    CHECK_EQ(rules::slugFromName("Science Fiction & Fantasy!"), "science-fiction-fantasy");
    CHECK_EQ(rules::slugFromName("  C++ 17  "), "c-17");
    CHECK_EQ(rules::slugFromName("Ιστορία"), "");
    CHECK_EQ(rules::validateSlug("history-greek"), "history-greek");
    expectError<ValidationError>([] { rules::validateSlug("Bad Slug"); }, "slug");
    expectError<ValidationError>([] { rules::validateSlug("-x"); }, "slug");
    expectError<ValidationError>([] { rules::validateSlug("a--b"); }, "slug");
}

TEST(tags_languages_and_lengths) {
    CHECK_EQ(rules::normalizeTag("  Space \t  Opera "), "space opera");
    CHECK_EQ(rules::normalizeTag("Ιστορία"), "Ιστορία");  // non-ASCII case left to the collation
    expectError<ValidationError>([] { rules::normalizeTag("   "); }, "tags");
    expectError<ValidationError>([] { rules::normalizeTag(std::string(51, 'a')); }, "tags");
    CHECK_EQ(rules::normalizeLanguage(" EL "), "el");
    expectError<ValidationError>([] { rules::normalizeLanguage("ell"); }, "language");
    CHECK_EQ(rules::charCount("Καλημέρα"), 8u);
    CHECK_EQ(rules::requiredText("name", std::string(200, 'x'), 200).size(), 200u);
    expectError<ValidationError>([] { rules::requiredText("name", std::string(201, 'x'), 200); }, "name");
}

// ---- Categories --------------------------------------------------------------

TEST(category_slug_derived_or_explicit) {
    Catalog c;
    CHECK_EQ(c.categories.create("Science Fiction").slug, "science-fiction");
    CHECK_EQ(c.categories.create("Ιστορία", "history").slug, "history");
    expectError<ValidationError>([&] { c.categories.create("Ποίηση"); }, "slug");
    expectError<ConflictError>([&] { c.categories.create("Science Fiction", "sf"); }, "name_taken");
    expectError<ConflictError>([&] { c.categories.create("SF", "history"); }, "slug_taken");
}

TEST(category_in_use_cannot_be_removed) {
    Catalog c;
    c.books.create(c.validBook());
    expectError<ConflictError>([&] { c.categories.remove(*c.sciFi); }, "category_in_use");
    expectError<NotFoundError>([&] { c.categories.remove(CategoryId(999)); }, "not_found");
}

// ---- Authors -----------------------------------------------------------------

TEST(author_optimistic_locking) {
    Catalog c;
    Author a = c.authors.create({"Ursula K. Le Guin", std::string("  "), Date(1929, 10, 21)});
    CHECK(!a.bio.has_value());  // blank becomes absent
    CHECK_EQ(a.version, 1);

    Author updated = c.authors.update(a.id, 1, {"Ursula K. Le Guin", std::string("Author of Earthsea"), a.birthDate});
    CHECK_EQ(updated.version, 2);
    CHECK_EQ(updated.bio.value(), "Author of Earthsea");

    expectError<ConflictError>([&] { c.authors.update(a.id, 1, {"Stale", std::nullopt, std::nullopt}); },
                               "version_conflict");
    expectError<NotFoundError>([&] { c.authors.update(AuthorId(999), 1, {"X", std::nullopt, std::nullopt}); },
                               "not_found");
}

TEST(author_validation) {
    Catalog c;
    expectError<ValidationError>([&] { c.authors.create({"  ", std::nullopt, std::nullopt}); }, "name");
    expectError<ValidationError>([&] { c.authors.create({"Future", std::nullopt, Date(2030, 1, 1)}); }, "birthDate");
}

TEST(author_with_books_cannot_be_removed) {
    Catalog c;
    c.books.create(c.validBook());
    expectError<ConflictError>([&] { c.authors.remove(*c.asimov); }, "author_has_books");
}

// ---- Books -------------------------------------------------------------------

TEST(book_create_normalizes_and_publishes_event) {
    Catalog c;
    BookDetails b = c.books.create(c.validBook());
    CHECK_EQ(b.title, "Foundation");
    CHECK_EQ(b.isbn.value(), "9780553293357");
    CHECK_EQ(b.language, "en");
    CHECK(b.tags == (std::vector<std::string>{"classic", "space opera"}));  // normalized, deduplicated
    CHECK_EQ(b.authors.at(0).name, "Isaac Asimov");
    CHECK_EQ(b.category.slug, "science-fiction");
    CHECK(b.createdAt == kNow);
    CHECK(!b.ratingAverage.has_value());

    CHECK_EQ(c.events->sent.size(), 1u);
    CHECK_EQ(c.events->sent[0].first, "catalog/books/" + std::to_string(b.id.value) + "/created");
    CHECK_EQ(c.events->sent[0].second, "Foundation");
}

TEST(book_validation_errors) {
    Catalog c;
    auto with = [&](auto change) {
        BookInput in = c.validBook();
        change(in);
        return [&c, in] { c.books.create(in); };
    };
    expectError<ValidationError>(with([](BookInput& in) { in.title = " "; }), "title");
    expectError<ValidationError>(with([](BookInput& in) { in.isbn = "123"; }), "isbn");
    expectError<ValidationError>(with([](BookInput& in) { in.language = "english"; }), "language");
    expectError<ValidationError>(with([](BookInput& in) { in.pageCount = 0; }), "pageCount");
    expectError<ValidationError>(with([](BookInput& in) { in.authors.clear(); }), "authors");
    expectError<ValidationError>(with([](BookInput& in) { in.authors.push_back(in.authors[0]); }), "authors");
    expectError<ValidationError>(with([](BookInput& in) { in.category = CategoryId(); }), "category");
    expectError<ValidationError>(with([](BookInput& in) {
                                     in.tags.clear();
                                     for (int i = 0; i < 21; ++i) in.tags.push_back("t" + std::to_string(i));
                                 }),
                                 "tags");
    CHECK(c.store.books.empty());
    CHECK(c.events->sent.empty());
}

TEST(book_unknown_references_roll_back_everything) {
    Catalog c;
    BookInput in = c.validBook();
    in.authors.push_back(AuthorId(777));
    in.authors.push_back(AuthorId(778));
    try {
        c.books.create(in);
        throw test::Failure{"expected ValidationError"};
    } catch (const ValidationError& e) {
        CHECK_EQ(e.field(), "authors");
        CHECK(std::string(e.what()).find("777, 778") != std::string::npos);
    }

    in = c.validBook();
    in.category = CategoryId(555);
    expectError<ValidationError>([&] { c.books.create(in); }, "category");

    CHECK(c.store.books.empty());
    CHECK(c.store.tags.empty());  // no tags left behind by the failed attempts
    CHECK(c.tx->rollbacks >= 2);
}

TEST(book_isbn_conflict) {
    Catalog c;
    c.books.create(c.validBook());
    BookInput again = c.validBook();
    again.isbn = "978-0-553-29335-7";  // same book, written as ISBN-13
    expectError<ConflictError>([&] { c.books.create(again); }, "isbn_taken");
}

TEST(book_update_replaces_fields_and_checks_version) {
    Catalog c;
    BookDetails b = c.books.create(c.validBook());
    BookInput in = c.validBook();
    in.title = "Foundation (Revised)";
    in.tags = {"robots"};
    BookDetails u = c.books.update(b.id, b.version, in);
    CHECK_EQ(u.title, "Foundation (Revised)");
    CHECK(u.tags == std::vector<std::string>{"robots"});
    CHECK_EQ(u.version, 2);
    CHECK_EQ(c.events->sent.back().first, "catalog/books/" + std::to_string(b.id.value) + "/updated");

    expectError<ConflictError>([&] { c.books.update(b.id, 1, in); }, "version_conflict");
    expectError<NotFoundError>([&] { c.books.update(BookId(999), 1, in); }, "not_found");
}

TEST(book_search_is_validated_and_normalized) {
    Catalog c;
    BookQuery q;
    q.tags = {" Space  Opera", "space opera", "Classic"};
    q.language = "EN";
    q.titleContains = "  found ";
    c.books.search(q);
    CHECK(c.bookRepo->lastQuery->tags == (std::vector<std::string>{"space opera", "classic"}));
    CHECK_EQ(c.bookRepo->lastQuery->language.value(), "en");
    CHECK_EQ(c.bookRepo->lastQuery->titleContains.value(), "found");
    CHECK(c.tx->options.back().readOnly);

    auto bad = [&](auto change) {
        BookQuery b;
        change(b);
        return [&c, b] { c.books.search(b); };
    };
    expectError<ValidationError>(bad([](BookQuery& b) { b.page.size = 0; }), "pageSize");
    expectError<ValidationError>(bad([](BookQuery& b) { b.page.size = 101; }), "pageSize");
    expectError<ValidationError>(bad([](BookQuery& b) { b.page.number = 0; }), "page");
    expectError<ValidationError>(bad([](BookQuery& b) { b.minRating = 5.5; }), "minRating");
    expectError<ValidationError>(bad([](BookQuery& b) {
                                     b.publishedFrom = Date(2020, 1, 1);
                                     b.publishedTo = Date(2019, 1, 1);
                                 }),
                                 "publishedFrom");
}

TEST(book_remove_publishes_and_removes_reviews) {
    Catalog c;
    BookDetails b = c.books.create(c.validBook());
    c.reviews.add(b.id, {"Maria", 5, std::nullopt, "Great"});
    c.books.remove(b.id);
    CHECK(c.store.reviews.empty());
    CHECK_EQ(c.events->sent.back().first, "catalog/books/" + std::to_string(b.id.value) + "/deleted");
    expectError<NotFoundError>([&] { c.books.remove(b.id); }, "not_found");
}

// ---- Reviews -----------------------------------------------------------------

TEST(reviews_keep_rating_totals) {
    Catalog c;
    BookDetails b = c.books.create(c.validBook());
    Review r1 = c.reviews.add(b.id, {"Maria", 5, std::string("Classic"), "Loved it"});
    c.reviews.add(b.id, {"Nikos", 2, std::nullopt, "Too slow"});
    CHECK_EQ(c.books.get(b.id).ratingCount, 2);
    CHECK_EQ(c.books.get(b.id).ratingAverage.value(), 3.5);
    CHECK_EQ(c.events->sent.back().first, "catalog/books/" + std::to_string(b.id.value) + "/reviews");
    CHECK_EQ(c.events->sent.back().second, "2 Nikos");

    c.reviews.update(r1.id, {"Maria", 3, std::nullopt, "On second thought"});
    CHECK_EQ(c.books.get(b.id).ratingAverage.value(), 2.5);

    c.reviews.remove(r1.id);
    CHECK_EQ(c.books.get(b.id).ratingCount, 1);
    CHECK_EQ(c.books.get(b.id).ratingAverage.value(), 2.0);
}

TEST(review_validation_and_missing_book) {
    Catalog c;
    BookDetails b = c.books.create(c.validBook());
    expectError<ValidationError>([&] { c.reviews.add(b.id, {"Maria", 0, std::nullopt, "x"}); }, "rating");
    expectError<ValidationError>([&] { c.reviews.add(b.id, {"Maria", 6, std::nullopt, "x"}); }, "rating");
    expectError<ValidationError>([&] { c.reviews.add(b.id, {" ", 4, std::nullopt, "x"}); }, "reviewerName");
    expectError<ValidationError>([&] { c.reviews.add(b.id, {"Maria", 4, std::nullopt, ""}); }, "body");
    expectError<NotFoundError>([&] { c.reviews.add(BookId(999), {"Maria", 4, std::nullopt, "x"}); }, "not_found");
    expectError<NotFoundError>([&] { c.reviews.listForBook(BookId(999), {}); }, "not_found");
    expectError<NotFoundError>([&] { c.reviews.update(ReviewId(999), {"M", 4, std::nullopt, "x"}); }, "not_found");
    CHECK(c.store.reviews.empty());
    CHECK_EQ(c.store.books.at(b.id.value).ratingCount, 0);
}

// ---- Reactions ---------------------------------------------------------------

namespace {
ReactionCounts periodOf(const ReactionStats& s, Period p) {
    for (const auto& [period, counts] : s.periods)
        if (period == p) return counts;
    throw test::Failure{"period missing"};
}
}  // namespace

TEST(reactions_are_buffered_until_flush) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    for (int i = 0; i < 3; ++i) c.reactions.record(b.id, Reaction::Like);
    c.reactions.record(b.id, Reaction::Dislike);
    CHECK_EQ(c.reactions.pending(), 1u);  // one (book, day) entry
    CHECK_EQ(periodOf(c.reactions.stats(b.id), Period::AllTime).likes, 0);

    auto r = c.reactions.flush();
    CHECK_EQ(r.written, 4);
    CHECK(!r.failed);
    CHECK_EQ(c.reactions.pending(), 0u);
    auto stats = c.reactions.stats(b.id);
    CHECK_EQ(periodOf(stats, Period::Today).likes, 3);
    CHECK_EQ(periodOf(stats, Period::Today).dislikes, 1);
    CHECK_EQ(periodOf(stats, Period::AllTime).score(), 2);
    CHECK_EQ(periodOf(stats, Period::Yesterday).likes, 0);
    CHECK_EQ(c.reactions.flush().written, 0);  // nothing pending
}

TEST(reactions_count_on_the_athens_day_they_happened) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    c.now = fromParts({Date(2026, 7, 1), 20, 59, 0, 0});  // 23:59 in Athens, 1 July
    c.reactions.record(b.id, Reaction::Like);
    c.now = fromParts({Date(2026, 7, 1), 21, 0, 0, 0});  // 00:00 in Athens, 2 July
    c.reactions.record(b.id, Reaction::Like);
    c.reactions.record(b.id, Reaction::Like);
    c.now = fromParts({Date(2026, 7, 2), 9, 0, 0, 0});  // flushed later: days already fixed
    c.reactions.flush();

    CHECK_EQ(c.store.dailyReactions.at({b.id.value, Date(2026, 7, 1).toDays()}).likes, 1);
    CHECK_EQ(c.store.dailyReactions.at({b.id.value, Date(2026, 7, 2).toDays()}).likes, 2);
    auto stats = c.reactions.stats(b.id);
    CHECK_EQ(periodOf(stats, Period::Today).likes, 2);
    CHECK_EQ(periodOf(stats, Period::Yesterday).likes, 1);
    CHECK_EQ(periodOf(stats, Period::Last7Days).likes, 3);
}

TEST(reaction_periods_are_rolling_windows_ending_today) {
    Catalog c;
    c.now = fromParts({Date(2026, 3, 10), 12, 0, 0, 0});
    auto days = [&](Period p) {
        auto r = c.reactions.range(p);
        return std::make_pair(r->from.toString(), r->to.toString());
    };
    CHECK(days(Period::Today) == std::make_pair(std::string("2026-03-10"), std::string("2026-03-10")));
    CHECK(days(Period::Yesterday) == std::make_pair(std::string("2026-03-09"), std::string("2026-03-09")));
    CHECK(days(Period::Last7Days) == std::make_pair(std::string("2026-03-04"), std::string("2026-03-10")));
    CHECK(days(Period::Last30Days) == std::make_pair(std::string("2026-02-09"), std::string("2026-03-10")));
    CHECK(days(Period::LastYear) == std::make_pair(std::string("2025-03-11"), std::string("2026-03-10")));
    CHECK(!c.reactions.range(Period::AllTime).has_value());
    CHECK(parsePeriod("last30Days") == Period::Last30Days);
    CHECK(!parsePeriod("forever").has_value());
}

TEST(old_daily_reactions_are_deleted_totals_kept) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    c.now = fromParts({Date(2025, 1, 10), 12, 0, 0, 0});  // old
    c.reactions.record(b.id, Reaction::Like);
    c.reactions.flush();
    c.now = fromParts({Date(2026, 1, 9), 12, 0, 0, 0});  // 364 days later
    c.reactions.record(b.id, Reaction::Like);
    c.reactions.flush();
    c.now = fromParts({Date(2026, 1, 20), 12, 0, 0, 0});
    // keep 366 days: 2025-01-20 and later stay; 2025-01-10 goes.
    CHECK_EQ(c.reactions.deleteOlderThan(366), 1);
    CHECK_EQ(c.store.dailyReactions.size(), 1u);
    CHECK_EQ(periodOf(c.reactions.stats(b.id), Period::AllTime).likes, 2);  // totals untouched
    CHECK_EQ(c.reactions.deleteOlderThan(366), 0);
    CHECK_THROWS_AS(c.reactions.deleteOlderThan(365), std::invalid_argument);

    const auto stats = c.reactions.bufferStats();
    CHECK_EQ(stats.pending, 0u);
    CHECK(stats.capacity > 0u);
}

TEST(failed_flush_keeps_reactions_for_the_next_one) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    c.reactions.record(b.id, Reaction::Like);
    c.reactionRepo->failNext = true;
    auto first = c.reactions.flush();
    CHECK(first.failed);
    c.reactions.record(b.id, Reaction::Like);  // arrives while the database is down
    auto second = c.reactions.flush();
    CHECK(!second.failed);
    CHECK_EQ(second.written, 2);
    CHECK_EQ(periodOf(c.reactions.stats(b.id), Period::AllTime).likes, 2);
}

TEST(reactions_only_for_books_that_accept_them) {
    Catalog c;
    BookInput in = c.validBook();
    in.reactionsEnabled = false;
    auto b = c.books.create(in);
    CHECK(!c.reactions.record(b.id, Reaction::Like));            // switched off
    CHECK(!c.reactions.record(BookId(424242), Reaction::Like));  // unknown
    CHECK_EQ(c.reactions.pending(), 0u);

    CHECK(c.books.setReactionsEnabled(b.id, true).reactionsEnabled);  // effective at once
    CHECK(c.reactions.record(b.id, Reaction::Like));
    c.books.setReactionsEnabled(b.id, false);
    CHECK(!c.reactions.record(b.id, Reaction::Like));
    CHECK_EQ(c.reactions.flush().written, 1);

    expectError<ValidationError>([&] { c.reactions.record(BookId(0), Reaction::Like); }, "bookId");
    expectError<NotFoundError>([&] { c.reactions.stats(BookId(424242)); }, "not_found");
    expectError<NotFoundError>([&] { c.books.setReactionsEnabled(BookId(424242), true); }, "not_found");
}

TEST(reactions_for_a_book_deleted_before_the_flush_are_skipped) {
    Catalog c;
    auto keep = c.books.create(c.validBook());
    BookInput other = c.validBook();
    other.isbn.reset();
    auto gone = c.books.create(other);
    c.reactions.record(keep.id, Reaction::Like);
    c.reactions.record(gone.id, Reaction::Like);
    c.reactions.record(gone.id, Reaction::Dislike);
    c.books.remove(gone.id);
    auto r = c.reactions.flush();
    CHECK_EQ(r.written, 1);
    CHECK_EQ(r.skipped, 2);
    CHECK(!c.reactions.record(gone.id, Reaction::Like));  // the cache forgot it
}

TEST(full_reaction_buffer_drops_new_entries_only) {
    Catalog c;
    std::vector<BookId> ids;
    for (int i = 0; i < 3; ++i) {
        BookInput in = c.validBook();
        in.isbn.reset();
        ids.push_back(c.books.create(in).id);
    }
    ReactionService small{c.reactionRepo, c.bookRepo, c.bookCache, c.tx, TimeZone::utc(), 2, c.clock};
    CHECK(small.record(ids[0], Reaction::Like));
    CHECK(small.record(ids[1], Reaction::Like));
    CHECK(!small.record(ids[2], Reaction::Like));  // third entry: dropped
    CHECK(small.record(ids[0], Reaction::Like));   // existing entry: still counted
    CHECK_EQ(small.pending(), 2u);
}

// ---- Book cache --------------------------------------------------------------

TEST(book_cache_follows_service_changes) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    CHECK_EQ(c.bookCache->get(b.id)->title, "Foundation");
    CHECK(c.bookCache->reactionsEnabled(b.id));

    BookInput in = c.validBook();
    in.title = "Foundation (Revised)";
    c.books.update(b.id, b.version, in);
    CHECK_EQ(c.bookCache->get(b.id)->title, "Foundation (Revised)");
    CHECK(c.bookCache->reactionsEnabled(b.id));  // update leaves the switch alone

    c.books.remove(b.id);
    CHECK(!c.bookCache->get(b.id).has_value());
}

TEST(book_cache_reload_picks_up_outside_changes) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    c.store.books.at(b.id.value).reactionsEnabled = false;  // edited behind the services' back
    c.store.books[999] = Book{};
    c.store.books[999].id = BookId(999);
    c.store.books[999].title = "Inserted by hand";
    CHECK(c.bookCache->reactionsEnabled(b.id));  // stale until the reload
    c.bookCache->reload();
    CHECK(!c.bookCache->reactionsEnabled(b.id));
    CHECK_EQ(c.bookCache->get(BookId(999))->title, "Inserted by hand");
    CHECK_EQ(c.bookCache->stats().size, 2u);
}

TEST(top_ranks_by_likes_minus_dislikes) {
    Catalog c;
    auto a = c.books.create(c.validBook());
    BookInput other = c.validBook();
    other.title = "The Gods Themselves";
    other.isbn.reset();
    auto b = c.books.create(other);
    for (int i = 0; i < 5; ++i) c.reactions.record(a.id, Reaction::Like);
    for (int i = 0; i < 3; ++i) c.reactions.record(a.id, Reaction::Dislike);  // +2
    for (int i = 0; i < 3; ++i) c.reactions.record(b.id, Reaction::Like);     // +3
    c.reactions.flush();

    auto liked = c.reactions.top(Period::Today, ReactionOrder::MostLiked, 10);
    CHECK_EQ(liked.at(0).title, "The Gods Themselves");
    CHECK_EQ(liked.at(1).counts.score(), 2);
    auto disliked = c.reactions.top(Period::AllTime, ReactionOrder::MostDisliked, 10);
    CHECK_EQ(disliked.size(), 1u);
    CHECK_EQ(disliked[0].title, "Foundation");
    expectError<ValidationError>([&] { c.reactions.top(Period::Today, ReactionOrder::MostLiked, 0); }, "limit");
    expectError<ValidationError>([&] { c.reactions.top(Period::Today, ReactionOrder::MostLiked, 101); }, "limit");
}

TEST(concurrent_records_and_flushes_lose_nothing) {
    Catalog c;
    auto b = c.books.create(c.validBook());
    constexpr int kThreads = 8, kEach = 2000;
    std::atomic<bool> done{false};
    std::thread flusher([&] {
        while (!done) c.reactions.flush();
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i)
                c.reactions.record(b.id, (t + i) % 4 == 0 ? Reaction::Dislike : Reaction::Like);
        });
    for (auto& th : threads) th.join();
    done = true;
    flusher.join();
    c.reactions.flush();
    auto all = periodOf(c.reactions.stats(b.id), Period::AllTime);
    CHECK_EQ(all.likes + all.dislikes, kThreads * kEach);
    CHECK_EQ(all.dislikes, kThreads * kEach / 4);
}

/// Runs every test case of this file (see TestHarness.hpp).
int main() { return test::runAll(); }
