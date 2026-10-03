/// @file
/// db::SqlExecutor: runs statements on the current transaction's connection or
/// a pooled one, with logging of slow queries.
/// @ingroup db

#include "caelitus/db/SqlExecutor.hpp"

#include "caelitus/db/DbErrors.hpp"
#include "caelitus/log/Log.hpp"
#include "db/detail/SqlText.hpp"
#include "db/detail/TransactionBinding.hpp"

#include <chrono>

namespace caelitus::db {

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t rowCount(const ExecResult& r) { return r.affectedRows; }
std::uint64_t rowCount(const ResultSet& r) { return r.size(); }

}  // namespace

SqlExecutor::SqlExecutor(std::shared_ptr<ConnectionPool> pool, ExecutorConfig config)
    : pool_(std::move(pool)),
      config_(config),
      log_(log::get("db.sql")) {
    if (!pool_) throw std::invalid_argument("SqlExecutor: pool is null");
}

template <typename F>
auto SqlExecutor::timed(std::string_view sql, F&& f) {
    const auto start = Clock::now();
    try {
        auto result = f();
        const double ms = millisSince(start);
        if (ms >= static_cast<double>(config_.slowQueryThreshold.count()))
            log_->warn("Slow SQL: {:.1f} ms (threshold {} ms), {} rows: {}", ms, config_.slowQueryThreshold.count(),
                       rowCount(result), detail::shortenSql(sql));
        else if (log_->should_log(spdlog::level::trace))
            log_->trace("SQL {:.1f} ms, {} rows: {}", ms, rowCount(result), detail::shortenSql(sql));
        return result;
    } catch (const DatabaseError& e) {
        log_->debug("SQL failed after {:.1f} ms: {}", millisSince(start), e.what());
        throw;
    }
}

template <typename F>
auto SqlExecutor::withConnection(F&& f, bool idempotent) {
    if (auto* tx = detail::currentTransaction(pool_.get())) {
        try {
            return f(*tx->conn);
        } catch (const DatabaseError& e) {
            // Deadlocks and lost connections abort the whole transaction on the
            // server. If the caller swallows this, it must still not commit.
            if (e.isTransient()) {
                tx = detail::currentTransaction(pool_.get());
                if (tx && !tx->rollbackOnly) {
                    tx->rollbackOnly = true;
                    tx->retryable = true;
                }
            }
            throw;
        }
    }

    // Autocommit. A read that failed on a dead connection is retried once on a
    // fresh one; a write is not, since it may already have been applied.
    try {
        PooledConnection conn = pool_->acquire();
        return f(*conn);
    } catch (const ConnectionError& e) {
        if (!idempotent || !e.isTransient()) throw;
        log_->warn("Read failed on a broken connection, retrying once: {}", e.what());
    }
    PooledConnection conn = pool_->acquire();
    return f(*conn);
}

ExecResult SqlExecutor::execute(std::string_view sql, const Params& params) {
    return withConnection([&](IConnection& c) { return timed(sql, [&] { return c.execute(sql, params); }); }, false);
}

ExecResult SqlExecutor::insert(std::string_view sql, const Params& params) {
    return withConnection([&](IConnection& c) { return timed(sql, [&] { return c.insert(sql, params); }); }, false);
}

ResultSet SqlExecutor::query(std::string_view sql, const Params& params) {
    return withConnection([&](IConnection& c) { return timed(sql, [&] { return c.query(sql, params); }); }, true);
}

std::optional<Row> SqlExecutor::queryOne(std::string_view sql, const Params& params) {
    auto rows = query(sql, params);
    if (rows.empty()) return std::nullopt;
    if (rows.size() > 1) throw DataMappingError("Expected at most one row, got " + std::to_string(rows.size()));
    return std::move(rows.front());
}

}  // namespace caelitus::db
