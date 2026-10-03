/// @file
/// Catalog services over the real MariaDB repositories. Skipped (exit 0)
/// unless CAELITUS_TEST_DB_HOST is set; see db_integration_tests.cpp.
/// Drops and recreates the catalog tables.
/// @ingroup tests

#include "TestHarness.hpp"

#include "caelitus/catalog/mariadb/CatalogMigrations.hpp"
#include "caelitus/catalog/mariadb/MariaDbReactionRepository.hpp"
#include "caelitus/catalog/mariadb/MariaDbRepositories.hpp"
#include "caelitus/catalog/service/AuthorService.hpp"
#include "caelitus/catalog/service/BookCache.hpp"
#include "caelitus/catalog/service/BookService.hpp"
#include "caelitus/catalog/service/CategoryService.hpp"
#include "caelitus/catalog/service/ReactionService.hpp"
#include "caelitus/catalog/service/ReviewService.hpp"
#include "caelitus/db/TransactionManager.hpp"
#include "caelitus/db/mariadb/MariaDbConnection.hpp"
#include "caelitus/log/Log.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace caelitus;
using namespace caelitus::catalog;

namespace {

std::string env(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

db::DbConfig testConfig() {
    db::DbConfig c;
    c.host = env("CAELITUS_TEST_DB_HOST", "127.0.0.1");
    c.port = static_cast<std::uint16_t>(std::stoi(env("CAELITUS_TEST_DB_PORT", "3306")));
    c.user = env("CAELITUS_TEST_DB_USER", "root");
    c.password = env("CAELITUS_TEST_DB_PASSWORD", "test");
    c.database = env("CAELITUS_TEST_DB_NAME", "caelitus_test");
    return c;
}

std::shared_ptr<db::ConnectionPool> newPool() {
    return db::ConnectionPool::create(std::make_shared<db::mariadb::MariaDbConnectionFactory>(testConfig()));
}

void dropCatalog(db::ConnectionPool& pool) {
    auto conn = pool.acquire();
    for (const char* t : {"book_reactions_daily", "reviews", "book_tags", "tags", "book_authors", "books", "authors",
                          "categories", "schema_migrations"})
        conn->execute(std::string("DROP TABLE IF EXISTS ") + t, {});
}

// Thread-safe, like every IMqttPublisher: services publish from many threads.
class RecordingPublisher final : public mqtt::IMqttPublisher {
public:
    std::string last() {
        std::lock_guard<std::mutex> lock(mutex_);
        return topics_.empty() ? "" : topics_.back();
    }

protected:
    bool doPublish(std::string_view topic, std::string_view, const mqtt::PublishOptions&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        topics_.emplace_back(topic);
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> topics_;
};

struct Catalog {
    std::shared_ptr<db::ConnectionPool> pool = newPool();
    std::shared_ptr<db::SqlExecutor> sql = std::make_shared<db::SqlExecutor>(pool);
    std::shared_ptr<db::TransactionManager> tx = std::make_shared<db::TransactionManager>(pool);
    std::shared_ptr<RecordingPublisher> events = std::make_shared<RecordingPublisher>();

    std::shared_ptr<mariadb::MariaDbCategoryRepository> categoryRepo =
        std::make_shared<mariadb::MariaDbCategoryRepository>(sql);
    std::shared_ptr<mariadb::MariaDbAuthorRepository> authorRepo =
        std::make_shared<mariadb::MariaDbAuthorRepository>(sql);
    std::shared_ptr<mariadb::MariaDbTagRepository> tagRepo = std::make_shared<mariadb::MariaDbTagRepository>(sql);
    std::shared_ptr<mariadb::MariaDbBookRepository> bookRepo = std::make_shared<mariadb::MariaDbBookRepository>(sql);
    std::shared_ptr<mariadb::MariaDbReviewRepository> reviewRepo =
        std::make_shared<mariadb::MariaDbReviewRepository>(sql);

    std::shared_ptr<BookCache> bookCache = std::make_shared<BookCache>(bookRepo);
    CategoryService categories{categoryRepo};
    AuthorService authors{authorRepo, tx};
    BookService books{bookRepo, authorRepo, categoryRepo, tagRepo, tx, events, systemClock(), bookCache};
    ReviewService reviews{reviewRepo, bookRepo, tx, events};
    std::shared_ptr<mariadb::MariaDbReactionRepository> reactionRepo =
        std::make_shared<mariadb::MariaDbReactionRepository>(sql);
    Timestamp now = fromParts({Date(2026, 10, 2), 9, 0, 0, 0});
    ReactionService reactions{reactionRepo,          bookRepo, bookCache, tx, TimeZone::named("Europe/Athens"), 100000,
                              [this] { return now; }};

    Catalog() {
        dropCatalog(*pool);
        db::MigrationRunner(pool).migrate(mariadb::catalogMigrations());
    }

    BookDetails book(const std::string& title, Date published, CategoryId cat, std::vector<AuthorId> authorIds,
                     std::vector<std::string> tags = {}, const std::string& lang = "en") {
        BookInput in;
        in.title = title;
        in.publishedOn = published;
        in.language = lang;
        in.category = cat;
        in.authors = std::move(authorIds);
        in.tags = std::move(tags);
        in.reactionsEnabled = true;
        return books.create(in);
    }
};

template <typename E>
std::string codeOf(const std::function<void()>& f) {
    try {
        f();
    } catch (const E& e) {
        return e.code();
    }
    return "<no exception>";
}

std::vector<std::string> titles(const Paged<BookSummary>& page) {
    std::vector<std::string> out;
    for (const auto& b : page.items) out.push_back(b.title);
    return out;
}

using Titles = std::vector<std::string>;

}  // namespace

// ---- Migrations --------------------------------------------------------------

TEST(migrations_apply_once_and_detect_tampering) {
    auto pool = newPool();
    dropCatalog(*pool);
    db::MigrationRunner runner(pool);
    auto migrations = mariadb::catalogMigrations();
    CHECK_EQ(runner.migrate(migrations), static_cast<int>(migrations.size()));
    CHECK_EQ(runner.migrate(migrations), 0);

    auto edited = migrations;
    edited[0].statements[0] += " COMMENT='edited'";
    CHECK_THROWS_AS(runner.migrate(edited), db::MigrationError);

    db::SqlExecutor(pool).execute("INSERT INTO schema_migrations VALUES (99, 'from the future', 'x', NOW())");
    CHECK_THROWS_AS(runner.migrate(migrations), db::MigrationError);
}

// ---- Books -------------------------------------------------------------------

TEST(book_round_trip) {
    Catalog c;
    auto sf = c.categories.create("Science Fiction");
    auto a = c.authors.create({"Larry Niven", std::nullopt, Date(1938, 4, 30)});
    auto b = c.authors.create({"Jerry Pournelle", std::nullopt, std::nullopt});

    BookInput in;
    in.title = "The Mote in God's Eye";
    in.isbn = "0-671-21833-6";
    in.description = "First contact.";
    in.publishedOn = Date(1974, 1, 1);
    in.language = "EN";
    in.pageCount = 537;
    in.category = sf.id;
    in.authors = {b.id, a.id};  // cover order, not id order
    in.tags = {"First Contact", "classic"};
    auto created = c.books.create(in);

    auto d = c.books.get(created.id);
    CHECK_EQ(d.title, "The Mote in God's Eye");
    CHECK_EQ(d.isbn.value(), "9780671218331");
    CHECK_EQ(d.pageCount.value(), 537);
    CHECK(d.publishedOn == Date(1974, 1, 1));
    CHECK_EQ(d.category.name, "Science Fiction");
    CHECK_EQ(d.authors.size(), 2u);
    CHECK_EQ(d.authors[0].name, "Jerry Pournelle");
    CHECK_EQ(d.authors[1].name, "Larry Niven");
    CHECK(d.tags == (Titles{"classic", "first contact"}));
    CHECK_EQ(d.version, 1);
    CHECK_EQ(c.events->last(), "catalog/books/" + std::to_string(d.id.value) + "/created");
}

TEST(search_filters_sorting_and_paging) {
    Catalog c;
    auto sf = c.categories.create("Science Fiction").id;
    auto hist = c.categories.create("Ιστορία", "history").id;
    auto asimov = c.authors.create({"Isaac Asimov", std::nullopt, std::nullopt}).id;
    auto leguin = c.authors.create({"Ursula Le Guin", std::nullopt, std::nullopt}).id;
    auto thuc = c.authors.create({"Θουκυδίδης", std::nullopt, std::nullopt}).id;

    auto foundation = c.book("Foundation", Date(1951, 6, 1), sf, {asimov}, {"classic", "space opera"});
    auto robots = c.book("I, Robot", Date(1950, 12, 2), sf, {asimov}, {"robots", "classic"});
    auto dispossessed = c.book("The Dispossessed", Date(1974, 5, 1), sf, {leguin}, {"utopia"});
    auto earthsea = c.book("A Wizard of Earthsea", Date(1968, 1, 1), sf, {leguin}, {"fantasy", "classic"});
    auto history = c.book("Ιστορία του Πελοποννησιακού Πολέμου", Date(1900, 1, 1), hist, {thuc}, {"classic"}, "el");
    c.book("Foundation and Empire", Date(1952, 1, 1), sf, {asimov}, {"space opera"});

    c.reviews.add(foundation.id, {"A", 5, std::nullopt, "x"});
    c.reviews.add(foundation.id, {"B", 4, std::nullopt, "x"});    // 4.5
    c.reviews.add(dispossessed.id, {"C", 5, std::nullopt, "x"});  // 5.0
    c.reviews.add(robots.id, {"D", 3, std::nullopt, "x"});        // 3.0

    BookQuery q;
    auto all = c.books.search(q);
    CHECK_EQ(all.total, 6);
    CHECK_EQ(all.items.front().title, "The Dispossessed");  // newest first
    CHECK_EQ(all.items.back().title, history.title);

    q = {};
    q.category = hist;
    CHECK(titles(c.books.search(q)) == Titles{history.title});

    q = {};
    q.author = leguin;
    q.sort = BookSort::TitleAsc;
    CHECK(titles(c.books.search(q)) == (Titles{"A Wizard of Earthsea", "The Dispossessed"}));

    q = {};
    q.tags = {"Space Opera", "robots"};  // any
    q.sort = BookSort::TitleAsc;
    CHECK(titles(c.books.search(q)) == (Titles{"Foundation", "Foundation and Empire", "I, Robot"}));

    q.tagMatch = TagMatch::All;
    q.tags = {"classic", "space opera"};
    CHECK(titles(c.books.search(q)) == Titles{"Foundation"});

    q = {};
    q.publishedFrom = Date(1951, 1, 1);
    q.publishedTo = Date(1968, 12, 31);
    q.sort = BookSort::PublishedAsc;
    CHECK(titles(c.books.search(q)) == (Titles{"Foundation", "Foundation and Empire", "A Wizard of Earthsea"}));

    q = {};
    q.titleContains = "FOUNDATION";  // case-insensitive collation
    CHECK_EQ(c.books.search(q).total, 2);

    q = {};
    q.language = "EL";
    CHECK(titles(c.books.search(q)) == Titles{history.title});

    q = {};
    q.minRating = 4.5;
    q.sort = BookSort::RatingDesc;
    CHECK(titles(c.books.search(q)) == (Titles{"The Dispossessed", "Foundation"}));

    q = {};
    q.sort = BookSort::RatingDesc;
    auto byRating = titles(c.books.search(q));
    CHECK_EQ(byRating.at(0), "The Dispossessed");
    CHECK_EQ(byRating.at(1), "Foundation");
    CHECK_EQ(byRating.at(2), "I, Robot");  // then the unrated ones

    q = {};
    q.sort = BookSort::TitleAsc;
    q.page = {2, 4};
    auto page2 = c.books.search(q);
    CHECK_EQ(page2.total, 6);
    CHECK_EQ(page2.pageCount(), 2);
    CHECK_EQ(page2.items.size(), 2u);
    q.page = {5, 4};
    auto beyond = c.books.search(q);
    CHECK(beyond.items.empty());
    CHECK_EQ(beyond.total, 6);

    // Summaries carry authors, tags and ratings without extra calls.
    q = {};
    q.titleContains = "Foundation";
    q.sort = BookSort::PublishedAsc;
    auto first = c.books.search(q).items.at(0);
    CHECK_EQ(first.authors.at(0).name, "Isaac Asimov");
    CHECK(first.tags == (Titles{"classic", "space opera"}));
    CHECK_EQ(first.ratingAverage.value(), 4.5);
    (void)earthsea;
}

TEST(like_wildcards_are_literal) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    c.book("100% Pure", Date(2000, 1, 1), cat, {a});
    c.book("1000 Pure", Date(2000, 1, 1), cat, {a});
    c.book("a_b", Date(2000, 1, 1), cat, {a});
    c.book("axb", Date(2000, 1, 1), cat, {a});
    BookQuery q;
    q.titleContains = "100%";
    CHECK(titles(c.books.search(q)) == Titles{"100% Pure"});
    q.titleContains = "a_b";
    CHECK(titles(c.books.search(q)) == Titles{"a_b"});
}

TEST(constraint_violations_become_domain_conflicts) {
    Catalog c;
    auto cat = c.categories.create("Science Fiction");
    auto a = c.authors.create({"Isaac Asimov", std::nullopt, std::nullopt});
    BookInput in;
    in.title = "Foundation";
    in.isbn = "9780553293357";
    in.publishedOn = Date(1951, 1, 1);
    in.language = "en";
    in.category = cat.id;
    in.authors = {a.id};
    c.books.create(in);

    in.title = "Another";
    in.isbn = "0-553-29335-4";  // same ISBN as ISBN-10
    CHECK_EQ(codeOf<ConflictError>([&] { c.books.create(in); }), "isbn_taken");
    CHECK_EQ(codeOf<ConflictError>([&] { c.authors.remove(a.id); }), "author_has_books");
    CHECK_EQ(codeOf<ConflictError>([&] { c.categories.remove(cat.id); }), "category_in_use");
    CHECK_EQ(codeOf<ConflictError>([&] { c.categories.create("Science Fiction", "sf"); }), "name_taken");
    CHECK_EQ(codeOf<ConflictError>([&] { c.categories.create("SF", "science-fiction"); }), "slug_taken");
}

TEST(optimistic_locking) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt});
    auto b = c.book("Draft", Date(2000, 1, 1), cat, {a.id});

    BookInput in;
    in.title = "Final";
    in.publishedOn = Date(2000, 1, 1);
    in.language = "en";
    in.category = cat;
    in.authors = {a.id};
    in.tags = {"edited"};
    auto u = c.books.update(b.id, 1, in);
    CHECK_EQ(u.version, 2);
    CHECK(u.tags == Titles{"edited"});
    CHECK_EQ(codeOf<ConflictError>([&] { c.books.update(b.id, 1, in); }), "version_conflict");
    CHECK_EQ(codeOf<NotFoundError>([&] { c.books.update(BookId(999999), 1, in); }), "not_found");

    c.authors.update(a.id, 1, {"Y", std::nullopt, std::nullopt});
    CHECK_EQ(codeOf<ConflictError>([&] { c.authors.update(a.id, 1, {"Z", std::nullopt, std::nullopt}); }),
             "version_conflict");
}

TEST(tags_are_shared_case_insensitively) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    c.book("One", Date(2000, 1, 1), cat, {a}, {"Ιστορία"});
    auto two = c.book("Two", Date(2000, 1, 1), cat, {a}, {"ιστορία", "SPACE"});
    CHECK(two.tags == (Titles{"space", "Ιστορία"}));  // existing spelling reused

    auto used = c.books.tags();
    CHECK_EQ(used.size(), 2u);
    CHECK_EQ(used[1].tag.name, "Ιστορία");
    CHECK_EQ(used[1].bookCount, 2);
}

TEST(deleting_a_book_removes_its_reviews) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    auto b = c.book("Gone", Date(2000, 1, 1), cat, {a}, {"t"});
    c.reviews.add(b.id, {"R", 4, std::nullopt, "ok"});
    c.books.remove(b.id);
    CHECK_EQ(c.sql->queryScalar<int>("SELECT COUNT(*) FROM reviews").value(), 0);
    CHECK_EQ(c.sql->queryScalar<int>("SELECT COUNT(*) FROM book_tags").value(), 0);
    c.authors.remove(a);  // no longer has books
}

// ---- Reactions ---------------------------------------------------------------

namespace {
ReactionCounts periodOf(const ReactionStats& s, Period p) {
    for (const auto& [period, counts] : s.periods)
        if (period == p) return counts;
    return {};
}
void react(Catalog& c, BookId id, int likes, int dislikes) {
    for (int i = 0; i < likes; ++i) c.reactions.record(id, Reaction::Like);
    for (int i = 0; i < dislikes; ++i) c.reactions.record(id, Reaction::Dislike);
}
}  // namespace

TEST(reaction_periods_and_rankings) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    auto dune = c.book("Dune", Date(1965, 1, 1), cat, {a}).id;
    auto solaris = c.book("Solaris", Date(1961, 1, 1), cat, {a}).id;
    auto meh = c.book("Meh", Date(2000, 1, 1), cat, {a}).id;

    c.now = fromParts({Date(2025, 12, 1), 12, 0, 0, 0});  // ~10 months ago
    react(c, dune, 50, 0);
    c.reactions.flush();
    c.now = fromParts({Date(2026, 10, 1), 12, 0, 0, 0});  // yesterday
    react(c, solaris, 10, 1);
    react(c, meh, 1, 6);
    c.reactions.flush();
    c.now = fromParts({Date(2026, 10, 2), 9, 0, 0, 0});  // today
    react(c, solaris, 3, 0);
    react(c, dune, 1, 2);
    react(c, meh, 0, 1);
    c.reactions.flush();
    react(c, solaris, 2, 0);  // same (book, day) again: upsert adds up
    c.reactions.flush();

    auto s = c.reactions.stats(solaris);
    CHECK_EQ(periodOf(s, Period::Today).likes, 5);
    CHECK_EQ(periodOf(s, Period::Yesterday).likes, 10);
    CHECK_EQ(periodOf(s, Period::Yesterday).dislikes, 1);
    CHECK_EQ(periodOf(s, Period::Last7Days).likes, 15);
    CHECK_EQ(periodOf(s, Period::AllTime).score(), 14);
    CHECK_EQ(periodOf(c.reactions.stats(dune), Period::LastYear).likes, 51);
    CHECK_EQ(periodOf(c.reactions.stats(dune), Period::Last30Days).likes, 1);

    auto titlesOf = [](const std::vector<RankedBook>& v) {
        Titles out;
        for (const auto& r : v) out.push_back(r.title);
        return out;
    };
    CHECK(titlesOf(c.reactions.top(Period::Today, ReactionOrder::MostLiked, 10)) == (Titles{"Solaris", "Dune"}));
    CHECK(titlesOf(c.reactions.top(Period::Today, ReactionOrder::MostDisliked, 10)) == (Titles{"Dune", "Meh"}));
    CHECK(titlesOf(c.reactions.top(Period::Last7Days, ReactionOrder::MostLiked, 10)) ==
          (Titles{"Solaris", "Dune", "Meh"}));
    CHECK(titlesOf(c.reactions.top(Period::AllTime, ReactionOrder::MostLiked, 10)) ==
          (Titles{"Dune", "Solaris", "Meh"}));
    CHECK(titlesOf(c.reactions.top(Period::AllTime, ReactionOrder::MostLiked, 1)) == Titles{"Dune"});
    CHECK(titlesOf(c.reactions.top(Period::AllTime, ReactionOrder::MostDisliked, 10)).front() == "Meh");

    CHECK_EQ(c.books.get(solaris).likes, 15);  // totals visible on the book too

    react(c, meh, 3, 0);  // buffered, then the book is deleted before the flush
    c.books.remove(meh);  // reactions go with the book
    CHECK_EQ(
        c.sql->queryScalar<int>("SELECT COUNT(*) FROM book_reactions_daily WHERE book_id = ?", {meh.value}).value(), 0);
    auto r = c.reactions.flush();
    CHECK_EQ(r.skipped, 3);
    CHECK_EQ(r.written, 0);
    CHECK(!c.reactions.record(meh, Reaction::Like));  // and no longer accepted
}

TEST(old_daily_reactions_are_deleted_in_sql) {
    Catalog c;
    auto cat = c.categories.create("Cleanup").id;
    auto a = c.authors.create({"Y", std::nullopt, std::nullopt}).id;
    auto b = c.book("Old news", Date(1990, 1, 1), cat, {a}).id;
    c.now = fromParts({Date(2024, 6, 1), 12, 0, 0, 0});
    react(c, b, 4, 1);
    c.reactions.flush();
    c.now = fromParts({Date(2026, 6, 1), 12, 0, 0, 0});
    react(c, b, 2, 0);
    c.reactions.flush();
    CHECK_EQ(c.reactions.deleteOlderThan(400), 1);
    CHECK_EQ(c.sql->queryScalar<int>("SELECT COUNT(*) FROM book_reactions_daily WHERE book_id = ?", {b.value}).value(),
             1);
    CHECK_EQ(c.books.get(b).likes, 6);  // all-time totals stay on the book
}

TEST(reaction_batches_larger_than_one_statement) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    // 1200 books in one go (MariaDB's sequence engine).
    c.sql->execute(
        "INSERT INTO books (title, published_on, language, category_id, created_at, updated_at)"
        " SELECT CONCAT('Book ', seq), '2000-01-01', 'en', ?, NOW(6), NOW(6) FROM seq_1_to_1200",
        {cat.value});
    c.sql->execute("UPDATE books SET reactions_enabled = TRUE");
    c.bookCache->reload();  // the rows were inserted behind the services' back
    auto ids =
        c.sql->queryList("SELECT id FROM books", {}, [](const db::Row& r) { return BookId(r.get<std::int64_t>(0)); });
    CHECK_EQ(ids.size(), 1200u);
    CHECK_EQ(c.bookCache->stats().size, 1200u);
    for (std::size_t i = 0; i < ids.size(); ++i) react(c, ids[i], 1 + static_cast<int>(i % 3), static_cast<int>(i % 2));
    auto r = c.reactions.flush();
    CHECK(!r.failed);
    CHECK_EQ(c.sql->queryScalar<std::int64_t>("SELECT SUM(likes) + SUM(dislikes) FROM books").value(), r.written);
    CHECK_EQ(c.sql->queryScalar<std::int64_t>("SELECT SUM(likes) + SUM(dislikes) FROM book_reactions_daily").value(),
             r.written);
    CHECK_EQ(c.sql->queryScalar<int>("SELECT COUNT(*) FROM book_reactions_daily").value(), 1200);
}

TEST(reactions_switch_is_stored_and_cached) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    BookInput in;
    in.title = "Quiet";
    in.publishedOn = Date(2000, 1, 1);
    in.language = "en";
    in.category = cat;
    in.authors = {a};
    auto b = c.books.create(in);  // reactionsEnabled defaults to false
    CHECK(!b.reactionsEnabled);
    CHECK(!c.reactions.record(b.id, Reaction::Like));

    auto on = c.books.setReactionsEnabled(b.id, true);
    CHECK(on.reactionsEnabled);
    CHECK_EQ(on.version, 1);
    CHECK(c.reactions.record(b.id, Reaction::Like));
    c.books.setReactionsEnabled(b.id, true);  // idempotent

    BookCache fresh(c.bookRepo);  // what a restart would load
    fresh.reload();
    CHECK(fresh.reactionsEnabled(b.id));
    CHECK_EQ(fresh.get(b.id)->title, "Quiet");
}

// ---- Concurrency -------------------------------------------------------------

TEST(concurrent_reviews_keep_exact_totals) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    auto b = c.book("Popular", Date(2000, 1, 1), cat, {a});

    constexpr int kThreads = 16, kEach = 10;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i) {
                try {
                    c.reviews.add(b.id, {"r" + std::to_string(t), 1 + (t + i) % 5, std::nullopt, "x"});
                } catch (const std::exception&) {
                    ++failures;
                }
            }
        });
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);

    int expectedSum = 0;
    for (int t = 0; t < kThreads; ++t)
        for (int i = 0; i < kEach; ++i) expectedSum += 1 + (t + i) % 5;
    auto d = c.books.get(b.id);
    CHECK_EQ(d.ratingCount, kThreads * kEach);
    CHECK_EQ(c.sql->queryScalar<int>("SELECT rating_sum FROM books WHERE id = ?", {b.id.value}).value(), expectedSum);
    CHECK_EQ(c.sql->queryScalar<int>("SELECT SUM(rating) FROM reviews").value(), expectedSum);
}

TEST(concurrent_updates_of_one_review_stay_consistent) {
    Catalog c;
    auto cat = c.categories.create("Misc").id;
    auto a = c.authors.create({"X", std::nullopt, std::nullopt}).id;
    auto b = c.book("Contested", Date(2000, 1, 1), cat, {a});
    auto r = c.reviews.add(b.id, {"R", 1, std::nullopt, "x"});

    // Without row locking, MariaDB 11's snapshot isolation rejects the racing
    // read-modify-writes (error 1020) until retries run out.
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 15; ++i) {
                try {
                    c.reviews.update(r.id, {"R", 1 + (t * 7 + i) % 5, std::nullopt, "x"});
                } catch (const std::exception&) {
                    ++failures;
                }
            }
        });
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);

    // Whatever update won last, the book's total must equal the review's rating.
    const int rating = c.reviews.get(r.id).rating;
    CHECK_EQ(c.sql->queryScalar<int>("SELECT rating_sum FROM books WHERE id = ?", {b.id.value}).value(), rating);
    CHECK_EQ(c.books.get(b.id).ratingCount, 1);
}

/// Runs every test case of this file (see TestHarness.hpp).
int main() {
    if (!std::getenv("CAELITUS_TEST_DB_HOST")) {
        std::cout << "CAELITUS_TEST_DB_HOST not set; skipping catalog integration tests\n";
        return 0;
    }
    log::LogConfig logConfig;
    logConfig.level = env("CAELITUS_TEST_LOG_LEVEL", "warn");
    log::init(logConfig);
    return test::runAll();
}
