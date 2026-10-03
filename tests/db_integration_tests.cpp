/// @file
/// Integration tests against a real MariaDB server. Skipped (exit 0) unless
/// CAELITUS_TEST_DB_HOST is set. Uses its own scratch tables.
///
/// @code{.sh}
/// docker run -d --name caelitus-test-db -p 3307:3306
///     -e MARIADB_ROOT_PASSWORD=test -e MARIADB_DATABASE=caelitus_test mariadb:11
/// CAELITUS_TEST_DB_HOST=127.0.0.1 CAELITUS_TEST_DB_PORT=3307 ./db_integration_tests
/// @endcode
/// @ingroup tests

#include "TestHarness.hpp"

#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/DbErrors.hpp"
#include "caelitus/db/SqlExecutor.hpp"
#include "caelitus/db/TransactionManager.hpp"
#include "caelitus/db/mariadb/MariaDbConnection.hpp"
#include "caelitus/log/Log.hpp"

#include <cstdlib>
#include <thread>

using namespace caelitus::db;
using caelitus::Timestamp;

namespace {

std::string env(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

DbConfig testConfig() {
    DbConfig c;
    c.host = env("CAELITUS_TEST_DB_HOST", "127.0.0.1");
    c.port = static_cast<std::uint16_t>(std::stoi(env("CAELITUS_TEST_DB_PORT", "3306")));
    c.user = env("CAELITUS_TEST_DB_USER", "root");
    c.password = env("CAELITUS_TEST_DB_PASSWORD", "test");
    c.database = env("CAELITUS_TEST_DB_NAME", "caelitus_test");
    return c;
}

struct Db {
    std::shared_ptr<ConnectionPool> pool =
        ConnectionPool::create(std::make_shared<mariadb::MariaDbConnectionFactory>(testConfig()));
    SqlExecutor sql{pool};
    TransactionManager tx{pool};

    Db() {
        sql.execute("DROP TABLE IF EXISTS it_dates");
        sql.execute("DROP TABLE IF EXISTS it_child");
        sql.execute("DROP TABLE IF EXISTS it_parent");
        sql.execute(
            "CREATE TABLE it_parent (id BIGINT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(50) NOT NULL UNIQUE,"
            " score DOUBLE NULL, active BOOLEAN NOT NULL DEFAULT 1, big BIGINT UNSIGNED NULL) ENGINE=InnoDB");
        sql.execute(
            "CREATE TABLE it_child (id BIGINT AUTO_INCREMENT PRIMARY KEY, parent_id BIGINT NOT NULL,"
            " FOREIGN KEY (parent_id) REFERENCES it_parent(id)) ENGINE=InnoDB");
    }
};

}  // namespace

TEST(insert_and_read_back_all_types) {
    Db db;
    auto r = db.sql.insert("INSERT INTO it_parent (name, score, active, big) VALUES (?, ?, ?, ?)",
                           {"ελληνικά ✓", 2.5, false, std::uint64_t{18446744073709551615ULL}});
    CHECK_EQ(r.affectedRows, 1u);
    CHECK(r.lastInsertId > 0);

    auto row = db.sql.queryOne("SELECT * FROM it_parent WHERE id = ?", {r.lastInsertId});
    CHECK(row.has_value());
    CHECK_EQ(row->get<std::string>("name"), "ελληνικά ✓");
    CHECK_EQ(row->get<double>("score"), 2.5);
    CHECK_EQ(row->get<bool>("active"), false);
    CHECK_EQ(row->get<std::uint64_t>("big"), 18446744073709551615ULL);
}

TEST(nulls_round_trip) {
    Db db;
    db.sql.execute("INSERT INTO it_parent (name, score) VALUES (?, ?)", {"n", std::optional<double>{}});
    auto row = db.sql.queryOne("SELECT score, big FROM it_parent WHERE name = ?", {"n"});
    CHECK(row->isNull("score"));
    CHECK(!row->getOptional<double>("big").has_value());
}

TEST(duplicate_key_is_translated) {
    Db db;
    db.sql.execute("INSERT INTO it_parent (name) VALUES (?)", {"a"});
    CHECK_THROWS_AS(db.sql.execute("INSERT INTO it_parent (name) VALUES (?)", {"a"}), DuplicateKeyError);
}

TEST(foreign_key_is_translated) {
    Db db;
    CHECK_THROWS_AS(db.sql.execute("INSERT INTO it_child (parent_id) VALUES (?)", {999}), ForeignKeyError);
}

TEST(syntax_error_is_query_error) {
    Db db;
    CHECK_THROWS_AS(db.sql.query("SELEC nope"), QueryError);
}

TEST(transaction_commit_and_rollback) {
    Db db;
    db.tx.inTransaction([&] { db.sql.execute("INSERT INTO it_parent (name) VALUES ('kept')"); });
    CHECK_THROWS_AS(db.tx.inTransaction([&] {
        db.sql.execute("INSERT INTO it_parent (name) VALUES ('discarded')");
        throw std::runtime_error("abort");
    }),
                    std::runtime_error);

    CHECK_EQ(db.sql.queryScalar<int>("SELECT COUNT(*) FROM it_parent WHERE name = 'kept'").value(), 1);
    CHECK_EQ(db.sql.queryScalar<int>("SELECT COUNT(*) FROM it_parent WHERE name = 'discarded'").value(), 0);
}

TEST(uncommitted_rows_are_invisible_to_other_connections) {
    Db db;
    db.tx.inTransaction([&] {
        db.sql.execute("INSERT INTO it_parent (name) VALUES ('pending')");
        int seenElsewhere = -1;
        std::thread other([&] { seenElsewhere = db.sql.queryScalar<int>("SELECT COUNT(*) FROM it_parent").value(); });
        other.join();
        CHECK_EQ(seenElsewhere, 0);
    });
}

/// Reads the row count twice inside one transaction while another thread
/// commits an insert in between; returns {first, second}.
std::pair<int, int> countAroundConcurrentInsert(Db& db, const TransactionOptions& options) {
    return db.tx.inTransaction(options, [&] {
        int first = db.sql.queryScalar<int>("SELECT COUNT(*) FROM it_parent").value();
        std::thread other([&] { db.sql.execute("INSERT INTO it_parent (name) VALUES (UUID())"); });
        other.join();
        int second = db.sql.queryScalar<int>("SELECT COUNT(*) FROM it_parent").value();
        return std::make_pair(first, second);
    });
}

TEST(read_committed_sees_concurrent_commits) {
    Db db;
    auto [first, second] = countAroundConcurrentInsert(db, {IsolationLevel::ReadCommitted});
    CHECK_EQ(first, 0);
    CHECK_EQ(second, 1);
}

TEST(repeatable_read_keeps_snapshot) {
    Db db;
    auto [first, second] = countAroundConcurrentInsert(db, {IsolationLevel::RepeatableRead});
    CHECK_EQ(first, 0);
    CHECK_EQ(second, 0);
}

TEST(isolation_applies_to_one_transaction_only) {
    Db db;
    countAroundConcurrentInsert(db, {IsolationLevel::ReadCommitted});
    // Same pooled connection, default (REPEATABLE READ) again.
    auto [first, second] = countAroundConcurrentInsert(db, {});
    CHECK_EQ(first, second);
}

TEST(read_only_transaction_rejects_writes) {
    Db db;
    CHECK_THROWS_AS(db.tx.inTransaction(TransactionOptions::readOnlyTx(),
                                        [&] { db.sql.execute("INSERT INTO it_parent (name) VALUES ('x')"); }),
                    QueryError);
    CHECK_EQ(db.sql.queryScalar<int>("SELECT COUNT(*) FROM it_parent").value(), 0);
}

// ---- Dates -------------------------------------------------------------------

/// (Re)creates the scratch table of the date and time round-trip tests.
void createDatesTable(Db& db) {
    db.sql.execute("DROP TABLE IF EXISTS it_dates");
    db.sql.execute(
        "CREATE TABLE it_dates (id INT PRIMARY KEY, d DATE NULL, dt DATETIME NULL, dt6 DATETIME(6) NULL,"
        " ts6 TIMESTAMP(6) NULL)");
}

TEST(dates_round_trip_through_all_column_types) {
    Db db;
    createDatesTable(db);
    const Timestamp precise = caelitus::fromParts({caelitus::Date(2026, 10, 2), 21, 47, 3, 123456});
    const Timestamp whole = caelitus::fromParts({caelitus::Date(1000, 1, 1), 0, 0, 0, 0});
    db.sql.execute("INSERT INTO it_dates VALUES (1, ?, ?, ?, ?)",
                   {caelitus::Date(2024, 2, 29), whole, precise, precise});

    auto row = db.sql.queryOne("SELECT * FROM it_dates WHERE id = 1");
    CHECK(row->get<caelitus::Date>("d") == caelitus::Date(2024, 2, 29));
    CHECK(row->get<Timestamp>("dt") == whole);
    CHECK(row->get<Timestamp>("dt6") == precise);
    CHECK(row->get<Timestamp>("ts6") == precise);
}

TEST(dates_null_and_lookup_by_date_parameter) {
    Db db;
    createDatesTable(db);
    db.sql.execute("INSERT INTO it_dates (id, d, dt6) VALUES (1, ?, ?), (2, ?, NULL)",
                   {caelitus::Date(2026, 1, 15), std::optional<Timestamp>{}, caelitus::Date(2026, 3, 1)});
    auto row = db.sql.queryOne("SELECT * FROM it_dates WHERE d < ?", {caelitus::Date(2026, 2, 1)});
    CHECK_EQ(row->get<int>("id"), 1);
    CHECK(!row->getOptional<Timestamp>("dt6").has_value());
}

TEST(session_is_utc) {
    Db db;
    // Server's NOW() agrees with our UTC clock.
    auto serverNow = db.sql.queryScalar<Timestamp>("SELECT NOW(6)").value();
    auto diff = serverNow - caelitus::nowUtc();
    CHECK(diff < std::chrono::seconds(5) && diff > std::chrono::seconds(-5));

    // TIMESTAMP columns convert through the session zone; UNIX_TIMESTAMP must
    // equal our epoch seconds for the value we stored.
    createDatesTable(db);
    const Timestamp ts = caelitus::fromParts({caelitus::Date(2026, 7, 1), 12, 0, 0, 0});
    db.sql.execute("INSERT INTO it_dates (id, ts6) VALUES (1, ?)", {ts});
    auto epoch = db.sql.queryScalar<std::int64_t>("SELECT FLOOR(UNIX_TIMESTAMP(ts6)) FROM it_dates").value();
    CHECK_EQ(epoch, std::chrono::duration_cast<std::chrono::seconds>(ts.time_since_epoch()).count());
}

TEST(far_future_sentinel_reads_as_timestamp) {
    Db db;
    auto ts = db.sql.queryScalar<Timestamp>("SELECT CAST('9999-12-31 23:59:59' AS DATETIME)").value();
    CHECK_EQ(caelitus::toIsoString(ts), "9999-12-31T23:59:59Z");
}

TEST(zero_date_is_a_mapping_error) {
    // Zero dates need a permissive sql_mode; use a private connection so the
    // pooled ones keep the default mode.
    auto conn = mariadb::MariaDbConnectionFactory(testConfig()).create();
    conn->execute("SET SESSION sql_mode = ''", {});
    auto rows = conn->query("SELECT CAST('0000-00-00' AS DATE) AS d", {});
    CHECK_THROWS_AS(rows.at(0).get<caelitus::Date>("d"), DataMappingError);
}

TEST(killed_connection_is_replaced) {
    Db db;
    auto id = db.sql.queryScalar<std::int64_t>("SELECT CONNECTION_ID()").value();
    {
        auto victim = db.pool->acquire();  // the idle connection whose id we read
        auto killer = db.pool->acquire();  // a second, fresh connection
        killer->execute("KILL " + std::to_string(id), {});
    }  // killer released first, so the pool (LIFO) hands out the dead victim next
    // A read on the dead connection is retried once on a fresh one.
    CHECK_EQ(db.sql.queryScalar<int>("SELECT 1").value(), 1);
}

TEST(bad_password_is_non_transient_connection_error) {
    auto cfg = testConfig();
    cfg.password = "wrong";
    mariadb::MariaDbConnectionFactory factory(cfg);
    bool caught = false;
    try {
        factory.create();
    } catch (const ConnectionError& e) {
        caught = true;
        CHECK(!e.isTransient());
    }
    CHECK(caught);
}

/// Runs every test case of this file (see TestHarness.hpp).
int main() {
    if (!std::getenv("CAELITUS_TEST_DB_HOST")) {
        std::cout << "CAELITUS_TEST_DB_HOST not set; skipping integration tests\n";
        return 0;
    }
    caelitus::log::LogConfig logConfig;
    logConfig.level = env("CAELITUS_TEST_LOG_LEVEL", "warn");
    caelitus::log::init(logConfig);
    return test::runAll();
}
