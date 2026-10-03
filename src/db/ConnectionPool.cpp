/// @file
/// db::ConnectionPool: borrowing and returning connections, validation of idle
/// ones, waiting with a timeout, statistics.
/// @ingroup db

#include "caelitus/db/ConnectionPool.hpp"

#include "caelitus/db/DbErrors.hpp"
#include "caelitus/log/Log.hpp"

namespace caelitus::db {

// ---- PooledConnection --------------------------------------------------------

PooledConnection& PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::move(other.pool_);
        conn_ = std::move(other.conn_);
    }
    return *this;
}

PooledConnection::~PooledConnection() { release(); }

void PooledConnection::release() noexcept {
    if (pool_ && conn_) pool_->release(std::move(conn_));
    pool_.reset();
    conn_.reset();
}

// ---- ConnectionPool ----------------------------------------------------------

std::shared_ptr<ConnectionPool> ConnectionPool::create(std::shared_ptr<IConnectionFactory> factory, PoolConfig config) {
    return std::shared_ptr<ConnectionPool>(new ConnectionPool(std::move(factory), config));
}

ConnectionPool::ConnectionPool(std::shared_ptr<IConnectionFactory> factory, PoolConfig config)
    : factory_(std::move(factory)),
      config_(config),
      log_(log::get("db.pool")) {
    if (!factory_) throw std::invalid_argument("ConnectionPool: factory is null");
    if (config_.maxSize == 0) throw std::invalid_argument("ConnectionPool: maxSize must be > 0");
}

PooledConnection ConnectionPool::acquire() {
    const auto deadline = Clock::now() + config_.acquireTimeout;
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;) {
        if (!idle_.empty()) {
            // LIFO: the most recently used connection is the most likely to be alive.
            IdleConnection item = std::move(idle_.back());
            idle_.pop_back();
            lock.unlock();

            const bool fresh = Clock::now() - item.since < config_.validateAfterIdle;
            if (fresh || item.conn->ping()) return PooledConnection(shared_from_this(), std::move(item.conn));

            log_->warn("Idle connection failed validation, replacing it");
            item.conn.reset();  // dead: close it outside the lock and try again
            discard();
            lock.lock();
            continue;
        }

        if (total_ < config_.maxSize) {
            ++total_;  // reserve the slot, then connect without holding the lock
            lock.unlock();
            return PooledConnection(shared_from_this(), open());
        }

        const bool ready =
            available_.wait_until(lock, deadline, [this] { return !idle_.empty() || total_ < config_.maxSize; });
        if (!ready) {
            log_->warn("Pool exhausted: no connection freed within {} ms (all {} in use)",
                       config_.acquireTimeout.count(), config_.maxSize);
            throw PoolTimeoutError("No database connection available within " +
                                   std::to_string(config_.acquireTimeout.count()) + " ms (pool size " +
                                   std::to_string(config_.maxSize) + ")");
        }
    }
}

void ConnectionPool::warmUp(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (total_ >= config_.maxSize) return;
            ++total_;
        }
        release(open());
    }
}

ConnectionPool::Stats ConnectionPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {total_, idle_.size()};
}

// Opens a connection for an already reserved slot; frees the slot on failure.
std::unique_ptr<IConnection> ConnectionPool::open() {
    try {
        auto conn = factory_->create();
        if (!conn) throw ConnectionError("Connection factory returned null");
        log_->debug("Opened connection ({} of max {})", stats().total, config_.maxSize);
        return conn;
    } catch (const std::exception& e) {
        log_->warn("Failed to open connection: {}", e.what());
        discard();
        throw;
    } catch (...) {
        discard();
        throw;
    }
}

void ConnectionPool::release(std::unique_ptr<IConnection> conn) noexcept {
    if (!conn) return;

    // Never hand out a connection with an open transaction.
    if (conn->inTransaction() && !conn->isBroken()) {
        log_->warn("Connection returned with an open transaction; rolling it back");
        try {
            conn->rollback();
        } catch (...) {
        }
    }

    if (conn->isBroken() || conn->inTransaction()) {
        log_->warn("Discarding {} connection", conn->isBroken() ? "broken" : "unrecoverable");
        conn.reset();
        discard();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_.push_back({std::move(conn), Clock::now()});
    }
    available_.notify_one();
}

void ConnectionPool::discard() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --total_;
    }
    available_.notify_one();
}

}  // namespace caelitus::db
