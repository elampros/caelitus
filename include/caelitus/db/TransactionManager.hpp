#pragma once

/// @file
/// Transactions on a ConnectionPool.
/// @ingroup db

#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/DbConfig.hpp"
#include "caelitus/db/ITransactionManager.hpp"

#include <memory>

/// @cond INTERNAL
namespace spdlog {
class logger;
}  // namespace spdlog
/// @endcond

namespace caelitus::db {

/// ITransactionManager on a ConnectionPool.
///
/// One transaction holds one pooled connection from BEGIN to COMMIT; while it
/// runs, SqlExecutor sends that thread's statements to it.
///
/// Behavior:
/// - **Retries**: if the work throws a transient DatabaseError (deadlock, lost
///   connection, pool timeout), the whole transaction is retried according to
///   TransactionConfig::retry, with exponential backoff.
/// - **Swallowed errors**: if a statement fails but the work catches the
///   exception and returns normally, the transaction is still rolled back, and
///   TransactionError (or TransientError, which is retried) is thrown.
/// - **Unknown outcome**: if the connection is lost during COMMIT, the server
///   may or may not have committed. That is never retried; TransactionError is
///   thrown.
///
/// Logs to "db.tx": begin/commit at trace, rollbacks at debug, retries, slow
/// transactions and swallowed errors at warn, unknown commit outcome at error.
class TransactionManager final : public ITransactionManager {
public:
    /// @throws std::invalid_argument if `pool` is null.
    explicit TransactionManager(std::shared_ptr<ConnectionPool> pool, TransactionConfig config = {});

protected:
    void run(const TransactionOptions& options, const std::function<void()>& work) override;

private:
    void runJoined(const TransactionOptions& options, const std::function<void()>& work);
    void runOnce(const TransactionOptions& options, const std::function<void()>& work);
    void rollbackQuietly(IConnection& conn) noexcept;

    std::shared_ptr<ConnectionPool> pool_;
    TransactionConfig config_;
    std::shared_ptr<spdlog::logger> log_;
};

}  // namespace caelitus::db
