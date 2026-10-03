// Unit tests for the database layer. No server needed: a scripted fake
// connection stands in for MariaDB.

#include "LogCapture.hpp"
#include "TestHarness.hpp"

#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/DbErrors.hpp"
#include "caelitus/db/SqlExecutor.hpp"
#include "caelitus/db/TransactionManager.hpp"
#include "caelitus/log/Log.hpp"
#include "db/mariadb/MariaDbErrorTranslator.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace caelitus::db;

namespace {

struct FakeServer {
    std::vector<std::string> log;  // "c<id>:<statement>"
    // Called for every statement (incl. BEGIN/COMMIT/ROLLBACK); may throw.
    std::function<void(const std::string&)> onStatement;
    bool pingOk = true;
    bool refuseConnections = false;
    int opened = 0;
    int closed = 0;
};

class FakeConnection final : public IConnection {
public:
    FakeConnection(FakeServer& server, int id) : server_(server), id_(id) {}
    ~FakeConnection() override { ++server_.closed; }

    ExecResult execute(std::string_view sql, const Params&) override {
        run(std::string(sql));
        return {1, 0};
    }
    ExecResult insert(std::string_view sql, const Params&) override {
        run(std::string(sql));
        return {1, 42};
    }

    ResultSet query(std::string_view sql, const Params&) override {
        run(std::string(sql));
        auto cols = std::make_shared<const ColumnSet>(std::vector<std::string>{"id", "name"});
        return {Row(cols, {std::string("7"), std::nullopt})};
    }

    void begin(const TransactionOptions& options) override {
        std::string stmt = "BEGIN";
        if (options.isolation != IsolationLevel::Default) stmt += std::string(" ") + toString(options.isolation);
        if (options.readOnly) stmt += " READ ONLY";
        run(stmt);
        inTx_ = true;
    }
    void commit() override {
        run("COMMIT");
        inTx_ = false;
    }
    void rollback() override {
        run("ROLLBACK");
        inTx_ = false;
    }
    bool inTransaction() const noexcept override { return inTx_; }
    bool ping() noexcept override { return server_.pingOk && !broken_; }
    bool isBroken() const noexcept override { return broken_; }

    int id() const { return id_; }

private:
    void run(const std::string& sql) {
        server_.log.push_back("c" + std::to_string(id_) + ":" + sql);
        try {
            if (server_.onStatement) server_.onStatement(sql);
        } catch (const ConnectionError&) {
            broken_ = true;
            throw;
        }
    }

    FakeServer& server_;
    int id_;
    bool inTx_ = false;
    bool broken_ = false;
};

class FakeFactory final : public IConnectionFactory {
public:
    explicit FakeFactory(FakeServer& server) : server_(server) {}
    std::unique_ptr<IConnection> create() override {
        if (server_.refuseConnections) throw ConnectionError("refused");
        return std::make_unique<FakeConnection>(server_, ++server_.opened);
    }

private:
    FakeServer& server_;
};

using test::CaptureSink;

std::shared_ptr<CaptureSink> g_logs = std::make_shared<CaptureSink>();

caelitus::log::LogConfig testLogConfig() {
    caelitus::log::LogConfig config;
    config.level = "trace";
    config.console = false;
    config.extraSinks = {g_logs};
    return config;
}

struct Fixture {
    FakeServer server;
    std::shared_ptr<ConnectionPool> pool;
    SqlExecutor sql;
    TransactionManager tx;

    explicit Fixture(PoolConfig pc = {}, RetryPolicy rp = {3, std::chrono::milliseconds(0), 1.0})
        : pool(ConnectionPool::create(std::make_shared<FakeFactory>(server), pc)),
          sql(pool),
          tx(pool, TransactionConfig{rp, std::chrono::milliseconds(2000)}) {
        g_logs->clear();
    }
};

std::vector<std::string> expect(std::initializer_list<const char*> items) { return {items.begin(), items.end()}; }

}  // namespace

// ---- DbValue -----------------------------------------------------------------

TEST(dbvalue_string_literal_is_string_not_bool) {
    DbValue v("abc");
    CHECK(std::holds_alternative<std::string>(v.storage()));
}

TEST(dbvalue_integer_widths) {
    CHECK(std::holds_alternative<std::int64_t>(DbValue(5).storage()));
    CHECK(std::holds_alternative<std::int64_t>(DbValue(short{5}).storage()));
    CHECK(std::holds_alternative<std::uint64_t>(DbValue(5u).storage()));
    CHECK(std::holds_alternative<bool>(DbValue(true).storage()));
    CHECK(std::holds_alternative<double>(DbValue(1.5f).storage()));
}

TEST(dbvalue_null_and_optional) {
    CHECK(DbValue(nullptr).isNull());
    CHECK(DbValue(std::optional<int>{}).isNull());
    CHECK(std::holds_alternative<std::int64_t>(DbValue(std::optional<int>{3}).storage()));
    CHECK(DbValue(static_cast<const char*>(nullptr)).isNull());
}

// ---- Row ---------------------------------------------------------------------

TEST(row_typed_access_and_errors) {
    auto cols = std::make_shared<const ColumnSet>(std::vector<std::string>{"id", "name", "price", "flag", "gone"});
    Row row(cols, {std::string("12"), std::string("abc"), std::string("9.5"), std::string("1"), std::nullopt});

    CHECK_EQ(row.get<int>("id"), 12);
    CHECK_EQ(row.get<std::int64_t>(0), 12);
    CHECK_EQ(row.get<std::string>("name"), "abc");
    CHECK_EQ(row.get<double>("price"), 9.5);
    CHECK_EQ(row.get<bool>("flag"), true);
    CHECK(row.isNull("gone"));
    CHECK(!row.getOptional<int>("gone").has_value());

    CHECK_THROWS_AS(row.get<int>("gone"), DataMappingError);     // NULL
    CHECK_THROWS_AS(row.get<int>("name"), DataMappingError);     // not a number
    CHECK_THROWS_AS(row.get<int>("price"), DataMappingError);    // trailing ".5"
    CHECK_THROWS_AS(row.get<int>("missing"), DataMappingError);  // unknown column
    CHECK_THROWS_AS(row.get<int>(9), DataMappingError);          // index out of range
}

TEST(row_integer_overflow_is_an_error) {
    auto cols = std::make_shared<const ColumnSet>(std::vector<std::string>{"v"});
    Row row(cols, {std::string("300")});
    CHECK_THROWS_AS(row.get<std::uint8_t>("v"), DataMappingError);
}

// ---- ConnectionPool ----------------------------------------------------------

TEST(pool_reuses_connections) {
    Fixture f;
    { auto c = f.pool->acquire(); }
    { auto c = f.pool->acquire(); }
    CHECK_EQ(f.server.opened, 1);
    CHECK_EQ(f.pool->stats().idle, 1u);
}

TEST(pool_times_out_when_exhausted) {
    Fixture f(PoolConfig{1, std::chrono::milliseconds(30), std::chrono::milliseconds(30000)});
    auto held = f.pool->acquire();
    CHECK_THROWS_AS(f.pool->acquire(), PoolTimeoutError);
}

TEST(pool_waiter_gets_released_connection) {
    Fixture f(PoolConfig{1, std::chrono::milliseconds(2000), std::chrono::milliseconds(30000)});
    auto held = std::make_unique<PooledConnection>(f.pool->acquire());
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        held.reset();
    });
    auto c = f.pool->acquire();
    releaser.join();
    CHECK_EQ(f.server.opened, 1);
}

TEST(pool_discards_broken_connections) {
    Fixture f;
    f.server.onStatement = [](const std::string&) { throw ConnectionError("gone away", 2006); };
    {
        auto c = f.pool->acquire();
        CHECK_THROWS_AS(c->execute("UPDATE t SET x = 1", {}), ConnectionError);
    }
    CHECK_EQ(f.pool->stats().total, 0u);
    CHECK_EQ(f.server.closed, 1);
}

TEST(pool_pings_stale_idle_connections) {
    Fixture f(PoolConfig{4, std::chrono::milliseconds(1000), std::chrono::milliseconds(0)});
    { auto c = f.pool->acquire(); }
    f.server.pingOk = false;
    { auto c = f.pool->acquire(); }  // stale one fails ping -> replaced
    CHECK_EQ(f.server.opened, 2);
    CHECK_EQ(f.pool->stats().total, 1u);
}

TEST(pool_rolls_back_leaked_transaction_on_release) {
    Fixture f;
    {
        auto c = f.pool->acquire();
        c->begin({});
    }
    CHECK(f.server.log == expect({"c1:BEGIN", "c1:ROLLBACK"}));
    CHECK_EQ(f.pool->stats().idle, 1u);
}

TEST(pool_frees_slot_when_connect_fails) {
    Fixture f(PoolConfig{1, std::chrono::milliseconds(30), std::chrono::milliseconds(30000)});
    f.server.refuseConnections = true;
    CHECK_THROWS_AS(f.pool->acquire(), ConnectionError);
    f.server.refuseConnections = false;
    auto c = f.pool->acquire();  // would time out if the slot had leaked
    CHECK_EQ(f.pool->stats().total, 1u);
}

TEST(pool_warm_up_opens_connections) {
    Fixture f(PoolConfig{2, std::chrono::milliseconds(30), std::chrono::milliseconds(30000)});
    f.pool->warmUp(5);
    CHECK_EQ(f.server.opened, 2);
    CHECK_EQ(f.pool->stats().idle, 2u);
}

TEST(pool_concurrent_use_never_exceeds_max) {
    Fixture f(PoolConfig{3, std::chrono::milliseconds(5000), std::chrono::milliseconds(30000)});
    std::atomic<int> inUse{0}, maxSeen{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                auto c = f.pool->acquire();
                int now = ++inUse;
                int prev = maxSeen.load();
                while (now > prev && !maxSeen.compare_exchange_weak(prev, now)) {
                }
                std::this_thread::yield();
                --inUse;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(maxSeen.load() <= 3);
    CHECK(f.server.opened <= 3);
}

// ---- SqlExecutor -------------------------------------------------------------

TEST(executor_autocommit_returns_connection) {
    Fixture f;
    auto r = f.sql.insert("INSERT INTO t VALUES (?)", {1});
    CHECK_EQ(r.lastInsertId, 42u);
    CHECK_EQ(f.pool->stats().idle, 1u);
}

TEST(executor_query_one_and_scalar) {
    Fixture f;
    auto row = f.sql.queryOne("SELECT id, name FROM t WHERE id = ?", {7});
    CHECK(row.has_value());
    CHECK_EQ(row->get<int>("id"), 7);
    CHECK_EQ(f.sql.queryScalar<int>("SELECT id FROM t").value(), 7);
}

TEST(executor_query_list_maps_rows) {
    Fixture f;
    auto ids = f.sql.queryList("SELECT id FROM t", {}, [](const Row& r) { return r.get<int>("id"); });
    CHECK(ids == std::vector<int>{7});
}

TEST(executor_retries_read_once_on_lost_connection) {
    Fixture f;
    int calls = 0;
    f.server.onStatement = [&](const std::string&) {
        if (++calls == 1) throw ConnectionError("lost", 2013);
    };
    auto rows = f.sql.query("SELECT 1");
    CHECK_EQ(rows.size(), 1u);
    CHECK_EQ(f.server.opened, 2);
}

TEST(executor_does_not_retry_write_on_lost_connection) {
    Fixture f;
    int calls = 0;
    f.server.onStatement = [&](const std::string&) {
        if (++calls == 1) throw ConnectionError("lost", 2013);
    };
    CHECK_THROWS_AS(f.sql.execute("UPDATE t SET x = 1"), ConnectionError);
    CHECK_EQ(calls, 1);
}

// ---- TransactionManager ------------------------------------------------------

TEST(tx_commits_on_success_and_uses_one_connection) {
    Fixture f;
    int value = f.tx.inTransaction([&] {
        f.sql.execute("INSERT a");
        f.sql.execute("INSERT b");
        return 5;
    });
    CHECK_EQ(value, 5);
    CHECK(f.server.log == expect({"c1:BEGIN", "c1:INSERT a", "c1:INSERT b", "c1:COMMIT"}));
    CHECK_EQ(f.pool->stats().idle, 1u);
}

TEST(tx_rolls_back_on_exception_and_rethrows) {
    Fixture f;
    CHECK_THROWS_AS(f.tx.inTransaction([&] {
        f.sql.execute("INSERT a");
        throw std::runtime_error("business rule");
    }),
                    std::runtime_error);
    CHECK(f.server.log == expect({"c1:BEGIN", "c1:INSERT a", "c1:ROLLBACK"}));
}

TEST(tx_retries_on_deadlock) {
    Fixture f;
    int attempts = 0;
    f.tx.inTransaction([&] {
        ++attempts;
        if (attempts < 3) throw TransientError("deadlock", 1213);
        f.sql.execute("INSERT a");
    });
    CHECK_EQ(attempts, 3);
    CHECK_EQ(f.server.log.back(), "c1:COMMIT");
}

TEST(tx_gives_up_after_max_attempts) {
    Fixture f;
    int attempts = 0;
    CHECK_THROWS_AS(f.tx.inTransaction([&] {
        ++attempts;
        throw TransientError("deadlock", 1213);
    }),
                    TransientError);
    CHECK_EQ(attempts, 3);
}

TEST(tx_does_not_retry_non_transient_errors) {
    Fixture f;
    int attempts = 0;
    CHECK_THROWS_AS(f.tx.inTransaction([&] {
        ++attempts;
        throw DuplicateKeyError("dup", 1062);
    }),
                    DuplicateKeyError);
    CHECK_EQ(attempts, 1);
}

TEST(tx_nested_calls_join_outer_transaction) {
    Fixture f;
    f.tx.inTransaction([&] {
        f.sql.execute("INSERT a");
        f.tx.inTransaction([&] { f.sql.execute("INSERT b"); });
    });
    CHECK(f.server.log == expect({"c1:BEGIN", "c1:INSERT a", "c1:INSERT b", "c1:COMMIT"}));
}

TEST(tx_swallowed_nested_failure_prevents_commit) {
    Fixture f;
    CHECK_THROWS_AS(f.tx.inTransaction([&] {
        f.sql.execute("INSERT a");
        try {
            f.tx.inTransaction([&] { throw std::runtime_error("inner"); });
        } catch (const std::exception&) {
            // ignored by (buggy) caller
        }
    }),
                    TransactionError);
    CHECK_EQ(f.server.log.back(), "c1:ROLLBACK");
}

TEST(tx_swallowed_deadlock_is_retried) {
    Fixture f;
    int attempts = 0;
    f.server.onStatement = [&](const std::string& sql) {
        if (sql == "INSERT a" && attempts == 1) throw TransientError("deadlock", 1213);
    };
    f.tx.inTransaction([&] {
        ++attempts;
        try {
            f.sql.execute("INSERT a");
        } catch (const DatabaseError&) {
            // ignored by caller; the server already rolled back
        }
    });
    CHECK_EQ(attempts, 2);
    CHECK_EQ(f.server.log.back(), "c1:COMMIT");
}

TEST(tx_caught_duplicate_key_does_not_abort) {
    Fixture f;
    f.server.onStatement = [](const std::string& sql) {
        if (sql == "INSERT dup") throw DuplicateKeyError("dup", 1062);
    };
    f.tx.inTransaction([&] {
        try {
            f.sql.execute("INSERT dup");
        } catch (const DuplicateKeyError&) {
            f.sql.execute("UPDATE instead");
        }
    });
    CHECK_EQ(f.server.log.back(), "c1:COMMIT");
}

TEST(tx_lost_connection_on_commit_is_not_retried) {
    Fixture f;
    int attempts = 0;
    f.server.onStatement = [](const std::string& sql) {
        if (sql == "COMMIT") throw ConnectionError("lost", 2013);
    };
    CHECK_THROWS_AS(f.tx.inTransaction([&] {
        ++attempts;
        f.sql.execute("INSERT a");
    }),
                    TransactionError);
    CHECK_EQ(attempts, 1);
    CHECK_EQ(f.pool->stats().total, 0u);  // broken connection dropped
}

TEST(tx_statements_outside_tx_use_other_connections) {
    Fixture f;
    f.tx.inTransaction([&] {
        f.sql.execute("INSERT a");
        std::thread other([&] { f.sql.execute("OTHER THREAD"); });
        other.join();
    });
    CHECK(f.server.log == expect({"c1:BEGIN", "c1:INSERT a", "c2:OTHER THREAD", "c1:COMMIT"}));
}

// ---- Isolation levels --------------------------------------------------------

TEST(tx_passes_isolation_and_read_only_to_connection) {
    Fixture f;
    f.tx.inTransaction({IsolationLevel::Serializable}, [&] { f.sql.query("SELECT 1"); });
    f.tx.inTransaction(TransactionOptions::readOnlyTx(), [&] { f.sql.query("SELECT 2"); });
    CHECK(f.server.log == expect({"c1:BEGIN SERIALIZABLE", "c1:SELECT 1", "c1:COMMIT", "c1:BEGIN READ ONLY",
                                  "c1:SELECT 2", "c1:COMMIT"}));
}

TEST(tx_nested_default_or_same_isolation_joins) {
    Fixture f;
    f.tx.inTransaction({IsolationLevel::ReadCommitted}, [&] {
        f.tx.inTransaction([&] { f.sql.execute("A"); });
        f.tx.inTransaction({IsolationLevel::ReadCommitted}, [&] { f.sql.execute("B"); });
    });
    CHECK(f.server.log == expect({"c1:BEGIN READ COMMITTED", "c1:A", "c1:B", "c1:COMMIT"}));
}

TEST(tx_nested_conflicting_isolation_is_rejected) {
    Fixture f;
    bool innerRan = false;
    CHECK_THROWS_AS(f.tx.inTransaction(
                        [&] { f.tx.inTransaction({IsolationLevel::Serializable}, [&] { innerRan = true; }); }),
                    TransactionError);
    CHECK(!innerRan);
    CHECK_EQ(f.server.log.back(), "c1:ROLLBACK");
}

// ---- Logging -----------------------------------------------------------------

TEST(log_retry_is_warned) {
    Fixture f;
    int attempts = 0;
    f.tx.inTransaction([&] {
        if (++attempts == 1) throw TransientError("deadlock", 1213);
    });
    CHECK(g_logs->contains(spdlog::level::warn, "db.tx: Transaction attempt 1/3 failed"));
}

TEST(log_slow_query_is_warned) {
    Fixture f;
    SqlExecutor strict(f.pool, ExecutorConfig{std::chrono::milliseconds(0)});
    strict.query("SELECT slow");
    CHECK(g_logs->contains(spdlog::level::warn, "Slow SQL"));
    CHECK(g_logs->contains(spdlog::level::warn, "SELECT slow"));
}

TEST(log_statements_traced_without_parameter_values) {
    Fixture f;
    f.sql.execute("INSERT INTO users (password) VALUES (?)", {"s3cret"});
    CHECK(g_logs->contains(spdlog::level::trace, "db.sql: SQL"));
    CHECK(!g_logs->contains(spdlog::level::trace, "s3cret"));
}

TEST(log_swallowed_error_is_warned) {
    Fixture f;
    try {
        f.tx.inTransaction([&] {
            try {
                f.tx.inTransaction([&] { throw std::runtime_error("inner"); });
            } catch (const std::exception&) {
            }
        });
    } catch (const TransactionError&) {
    }
    CHECK(g_logs->contains(spdlog::level::warn, "caught and ignored"));
}

TEST(log_broken_connection_discard_is_warned) {
    Fixture f;
    f.server.onStatement = [](const std::string&) { throw ConnectionError("gone away", 2006); };
    CHECK_THROWS_AS(f.sql.execute("UPDATE t SET x = 1"), ConnectionError);
    CHECK(g_logs->contains(spdlog::level::warn, "db.pool: Discarding broken connection"));
}

TEST(log_levels_are_hierarchical) {
    auto config = testLogConfig();
    config.level = "info";
    config.levels = {{"db", "warn"}, {"db.sql", "trace"}};
    caelitus::log::init(config);
    CHECK_EQ(caelitus::log::get("db.sql")->level(), spdlog::level::trace);
    CHECK_EQ(caelitus::log::get("db.pool")->level(), spdlog::level::warn);
    CHECK_EQ(caelitus::log::get("db.pool.extra")->level(), spdlog::level::warn);
    CHECK_EQ(caelitus::log::get("mqtt")->level(), spdlog::level::info);
    caelitus::log::init(testLogConfig());
}

TEST(log_unknown_level_is_rejected) {
    auto config = testLogConfig();
    config.level = "verbose";
    CHECK_THROWS_AS(caelitus::log::init(config), std::invalid_argument);
}

// ---- MariaDB error translation -----------------------------------------------

TEST(translator_maps_error_codes) {
    using caelitus::db::mariadb::throwTranslated;
    CHECK_THROWS_AS(throwTranslated(1062, "23000", "ctx", "dup"), DuplicateKeyError);
    CHECK_THROWS_AS(throwTranslated(1452, "23000", "ctx", "fk"), ForeignKeyError);
    CHECK_THROWS_AS(throwTranslated(1048, "23000", "ctx", "not null"), ConstraintViolationError);
    CHECK_THROWS_AS(throwTranslated(1213, "40001", "ctx", "deadlock"), TransientError);
    CHECK_THROWS_AS(throwTranslated(1205, "HY000", "ctx", "lock wait"), TransientError);
    CHECK_THROWS_AS(throwTranslated(2006, "HY000", "ctx", "gone"), ConnectionError);
    CHECK_THROWS_AS(throwTranslated(0, "08S01", "ctx", "link"), ConnectionError);
    CHECK_THROWS_AS(throwTranslated(1064, "42000", "ctx", "syntax"), QueryError);

    bool caught = false;
    try {
        throwTranslated(1045, "28000", "ctx", "denied");
    } catch (const ConnectionError& e) {
        caught = true;
        CHECK(!e.isTransient());
        CHECK_EQ(e.code(), 1045);
    }
    CHECK(caught);
}

int main() {
    caelitus::log::init(testLogConfig());
    return test::runAll();
}
