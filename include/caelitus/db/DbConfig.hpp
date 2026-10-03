#pragma once

/// @file
/// Settings of the database layer (the "db" configuration section).
/// @ingroup db

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace caelitus::db {

/// How to reach the database server ("db.connection").
struct DbConfig {
    std::string host = "127.0.0.1";  ///< Server host name or address.
    std::uint16_t port = 3306;       ///< Server TCP port.
    std::string user;                ///< Account name.
    std::string password;            ///< Account password; use "${ENV_VAR}" in the configuration file.
    std::string database;            ///< Default schema.

    /// Max time to establish a connection.
    std::chrono::milliseconds connectTimeout{5000};
    /// Max time to wait for a server response; 0 disables it.
    std::chrono::milliseconds socketTimeout{30000};

    /// Session time zone.
    ///
    /// Timestamp values are UTC, so this must stay UTC unless you know exactly
    /// why not: it decides how TIMESTAMP columns and NOW()/CURRENT_TIMESTAMP
    /// are converted. Empty: leave the server default.
    std::string sessionTimeZone = "+00:00";

    /// Executed on every new connection, after charset and time zone setup.
    std::vector<std::string> initStatements;
};

/// Connection pool limits ("db.pool"); see ConnectionPool.
struct PoolConfig {
    /// Most connections open at once. Callers beyond that wait.
    std::size_t maxSize = 8;
    /// How long ConnectionPool::acquire() waits for a free connection before
    /// throwing PoolTimeoutError.
    std::chrono::milliseconds acquireTimeout{5000};
    /// Idle connections older than this are pinged before being handed out.
    std::chrono::milliseconds validateAfterIdle{30000};
};

/// How often, and how patiently, a transaction is retried after a transient
/// error (deadlock, lost connection, ...).
struct RetryPolicy {
    int maxAttempts = 3;                           ///< Total attempts, including the first one.
    std::chrono::milliseconds initialBackoff{20};  ///< Wait before the second attempt.
    double backoffMultiplier = 2.0;                ///< Each further wait is this much longer.
};

/// SqlExecutor settings ("db.executor").
struct ExecutorConfig {
    /// Statements taking at least this long are logged as warnings.
    std::chrono::milliseconds slowQueryThreshold{500};
};

/// TransactionManager settings ("db.transaction").
struct TransactionConfig {
    RetryPolicy retry;  ///< Retries of transient failures.
    /// Transactions held open at least this long are logged as warnings
    /// (long transactions hold locks and block other writers).
    std::chrono::milliseconds slowTransactionThreshold{2000};
};

}  // namespace caelitus::db
