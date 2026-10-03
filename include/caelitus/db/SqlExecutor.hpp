#pragma once

/// @file
/// Running SQL statements.
/// @ingroup db

#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/DbConfig.hpp"
#include "caelitus/db/DbValue.hpp"
#include "caelitus/db/Row.hpp"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/// @cond INTERNAL
namespace spdlog {
class logger;
}  // namespace spdlog
/// @endcond

namespace caelitus::db {

/// What repositories use to talk to the database.
///
/// Inside ITransactionManager::inTransaction() every statement runs on the
/// transaction's connection; outside it, each statement borrows a pooled
/// connection in autocommit mode. Outside a transaction, a read that fails on
/// a dead connection is retried once on a fresh one; a write is not, since it
/// may already have been applied.
///
/// Always pass values through `params` (`?` placeholders); never build SQL by
/// concatenating input:
///
/// @code
/// auto book = sql.queryOne("SELECT id, title FROM books WHERE isbn = ?", {isbn});
/// auto count = sql.queryScalar<std::int64_t>("SELECT COUNT(*) FROM books");
/// auto titles = sql.queryList("SELECT title FROM books WHERE category_id = ?", {categoryId},
///                             [](const db::Row& r) { return r.get<std::string>("title"); });
/// @endcode
///
/// All methods throw DatabaseError subclasses only. Logs to "db.sql": every
/// statement with its duration at trace, failures at debug (the exception
/// carries the details), slow statements at warn.
class SqlExecutor {
public:
    /// @throws std::invalid_argument if `pool` is null.
    explicit SqlExecutor(std::shared_ptr<ConnectionPool> pool, ExecutorConfig config = {});

    /// Runs an INSERT, UPDATE, DELETE or DDL statement.
    /// @return The number of affected rows.
    ExecResult execute(std::string_view sql, const Params& params = {});
    /// Runs a single-row INSERT whose generated id you need (ExecResult::lastInsertId).
    ExecResult insert(std::string_view sql, const Params& params = {});
    /// Runs a SELECT and returns all rows.
    ResultSet query(std::string_view sql, const Params& params = {});

    /// Runs a SELECT that returns at most one row.
    /// @return The row, or std::nullopt if there is none.
    /// @throws DataMappingError if there are more rows.
    std::optional<Row> queryOne(std::string_view sql, const Params& params = {});

    /// The first column of the single result row, converted to T.
    /// @return std::nullopt if there is no row or the value is NULL.
    template <typename T>
    std::optional<T> queryScalar(std::string_view sql, const Params& params = {}) {
        auto row = queryOne(sql, params);
        if (!row) return std::nullopt;
        return row->getOptional<T>(0);
    }

    /// Runs a SELECT and maps every row with `mapper(const Row&) -> T`.
    /// @return The mapped rows, in result order.
    template <typename Mapper>
    auto queryList(std::string_view sql, const Params& params,
                   Mapper&& mapper) -> std::vector<std::invoke_result_t<Mapper&, const Row&>> {
        std::vector<std::invoke_result_t<Mapper&, const Row&>> out;
        auto rows = query(sql, params);
        out.reserve(rows.size());
        for (const auto& row : rows) out.push_back(mapper(row));
        return out;
    }

private:
    template <typename F>
    auto withConnection(F&& f, bool idempotent);
    template <typename F>
    auto timed(std::string_view sql, F&& f);

    std::shared_ptr<ConnectionPool> pool_;
    ExecutorConfig config_;
    std::shared_ptr<spdlog::logger> log_;
};

}  // namespace caelitus::db
