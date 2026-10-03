#pragma once

/// @file
/// The connection pool.
/// @ingroup db

#include "caelitus/db/DbConfig.hpp"
#include "caelitus/db/IConnection.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

/// @cond INTERNAL
namespace spdlog {
class logger;
}  // namespace spdlog
/// @endcond

namespace caelitus::db {

class ConnectionPool;

/// RAII handle to a pooled connection; returns it to the pool when destroyed.
///
/// Move-only. Keeps the pool alive while it exists. A connection returned with
/// an open transaction is rolled back first; one that reports
/// IConnection::isBroken() (or cannot be rolled back) is closed instead of
/// being reused.
class PooledConnection {
public:
    /// Takes over the connection; `other` becomes empty.
    PooledConnection(PooledConnection&&) noexcept = default;
    /// Returns the currently held connection first, then takes `other`'s.
    PooledConnection& operator=(PooledConnection&& other) noexcept;
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    /// Returns the connection to the pool.
    ~PooledConnection();

    IConnection* operator->() const noexcept { return conn_.get(); }  ///< The connection.
    IConnection& operator*() const noexcept { return *conn_; }        ///< The connection.
    IConnection* get() const noexcept { return conn_.get(); }         ///< The connection; null after a move.

private:
    friend class ConnectionPool;
    PooledConnection(std::shared_ptr<ConnectionPool> pool, std::unique_ptr<IConnection> conn)
        : pool_(std::move(pool)),
          conn_(std::move(conn)) {}
    void release() noexcept;

    std::shared_ptr<ConnectionPool> pool_;
    std::unique_ptr<IConnection> conn_;
};

/// Thread-safe, bounded pool of database connections.
///
/// Connections are opened lazily (up to PoolConfig::maxSize), pinged before
/// reuse once they have been idle for PoolConfig::validateAfterIdle, and
/// discarded once they report IConnection::isBroken(). Logs to "db.pool".
///
/// Repositories do not use the pool directly: SqlExecutor and
/// TransactionManager acquire connections for them.
class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
public:
    /// Pool size, for monitoring.
    struct Stats {
        std::size_t total = 0;  ///< Open connections: idle + in use + being opened.
        std::size_t idle = 0;   ///< Connections waiting to be handed out.
    };

    /// Creates a pool; no connection is opened yet (see warmUp()).
    /// @param factory  Opens connections; decides which database is used.
    /// @param config   Size and timeouts.
    static std::shared_ptr<ConnectionPool> create(std::shared_ptr<IConnectionFactory> factory, PoolConfig config = {});

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    /// Hands out an idle connection, opens a new one if below the limit, or
    /// waits up to PoolConfig::acquireTimeout for one to be returned.
    /// @throws PoolTimeoutError if none became free in time.
    /// @throws ConnectionError if a new connection could not be opened.
    PooledConnection acquire();

    /// Opens `count` connections up front (capped at PoolConfig::maxSize), so
    /// a wrong configuration or an unreachable server fails at startup.
    /// @throws ConnectionError
    void warmUp(std::size_t count);

    /// Current number of open and idle connections.
    Stats stats() const;

private:
    friend class PooledConnection;
    using Clock = std::chrono::steady_clock;

    struct IdleConnection {
        std::unique_ptr<IConnection> conn;
        Clock::time_point since;
    };

    ConnectionPool(std::shared_ptr<IConnectionFactory> factory, PoolConfig config);

    std::unique_ptr<IConnection> open();
    void release(std::unique_ptr<IConnection> conn) noexcept;
    void discard() noexcept;

    std::shared_ptr<IConnectionFactory> factory_;
    PoolConfig config_;
    std::shared_ptr<spdlog::logger> log_;

    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<IdleConnection> idle_;
    std::size_t total_ = 0;  // idle + in use + being opened
};

}  // namespace caelitus::db
