/// @file
/// db::TransactionManager: begin/commit/rollback, nesting, and retries of
/// transactions that failed on deadlocks or lost connections.
/// @ingroup db

#include "caelitus/db/TransactionManager.hpp"

#include "caelitus/db/DbErrors.hpp"
#include "caelitus/log/Log.hpp"
#include "db/detail/TransactionBinding.hpp"

#include <thread>

namespace caelitus::db {

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string describe(const TransactionOptions& options) {
    std::string out = toString(options.isolation);
    if (options.readOnly) out += ", READ ONLY";
    return out;
}

}  // namespace

TransactionManager::TransactionManager(std::shared_ptr<ConnectionPool> pool, TransactionConfig config)
    : pool_(std::move(pool)),
      config_(config),
      log_(log::get("db.tx")) {
    if (!pool_) throw std::invalid_argument("TransactionManager: pool is null");
}

void TransactionManager::run(const TransactionOptions& options, const std::function<void()>& work) {
    if (detail::currentTransaction(pool_.get())) {
        runJoined(options, work);
        return;
    }

    const RetryPolicy& retry = config_.retry;
    auto backoff = retry.initialBackoff;
    for (int attempt = 1;; ++attempt) {
        try {
            runOnce(options, work);
            return;
        } catch (const DatabaseError& e) {
            if (!e.isTransient()) throw;
            if (attempt >= retry.maxAttempts) {
                log_->warn("Transaction failed after {} attempts: {}", attempt, e.what());
                throw;
            }
            log_->warn("Transaction attempt {}/{} failed, retrying in {} ms: {}", attempt, retry.maxAttempts,
                       backoff.count(), e.what());
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::chrono::duration_cast<std::chrono::milliseconds>(backoff * retry.backoffMultiplier);
    }
}

// Nested call: the outer transaction owns begin/commit/retry. If the nested
// work fails, the outer one must not commit even if the caller swallows the
// exception, so mark it rollback-only.
void TransactionManager::runJoined(const TransactionOptions& options, const std::function<void()>& work) {
    auto markFailed = [this](bool transient) {
        auto* tx = detail::currentTransaction(pool_.get());
        if (tx && !tx->rollbackOnly) {
            tx->rollbackOnly = true;
            tx->retryable = transient;
        }
    };

    const IsolationLevel outer = detail::currentTransaction(pool_.get())->options.isolation;
    if (options.isolation != IsolationLevel::Default && options.isolation != outer) {
        markFailed(false);
        throw TransactionError(std::string("Nested transaction requests isolation ") + toString(options.isolation) +
                               " but the enclosing transaction uses " + toString(outer));
    }

    try {
        work();
    } catch (const DatabaseError& e) {
        markFailed(e.isTransient());
        throw;
    } catch (...) {
        markFailed(false);
        throw;
    }
}

void TransactionManager::runOnce(const TransactionOptions& options, const std::function<void()>& work) {
    PooledConnection conn = pool_->acquire();
    const auto start = Clock::now();
    conn->begin(options);
    log_->trace("BEGIN ({})", describe(options));

    bool rollbackOnly = false;
    bool retryable = false;
    {
        detail::BindingGuard guard(pool_.get(), conn.get(), options);
        try {
            work();
        } catch (const std::exception& e) {
            log_->debug("Rolling back after {:.1f} ms: {}", millisSince(start), e.what());
            rollbackQuietly(*conn);
            throw;
        } catch (...) {
            log_->debug("Rolling back after {:.1f} ms: non-standard exception", millisSince(start));
            rollbackQuietly(*conn);
            throw;
        }
        rollbackOnly = guard.binding().rollbackOnly;
        retryable = guard.binding().retryable;
    }

    if (rollbackOnly) {
        log_->warn("Transaction rolled back: an error inside it was caught and ignored by the caller");
        conn->rollback();
        if (retryable)
            throw TransientError("Transaction aborted: a transient database error inside it was caught and ignored");
        throw TransactionError("Transaction rolled back: an error inside it was caught and ignored");
    }

    try {
        conn->commit();
    } catch (const ConnectionError& e) {
        // The server may or may not have applied the commit: never retry.
        log_->error("Connection lost during COMMIT, outcome unknown: {}", e.what());
        throw TransactionError(std::string("Connection lost during COMMIT, outcome unknown: ") + e.what(), e.code(),
                               e.sqlState());
    } catch (...) {
        rollbackQuietly(*conn);
        throw;
    }

    const double ms = millisSince(start);
    if (ms >= static_cast<double>(config_.slowTransactionThreshold.count()))
        log_->warn("Slow transaction: committed after {:.1f} ms (threshold {} ms)", ms,
                   config_.slowTransactionThreshold.count());
    else log_->trace("COMMIT after {:.1f} ms", ms);
}

void TransactionManager::rollbackQuietly(IConnection& conn) noexcept {
    try {
        conn.rollback();
    } catch (const std::exception& e) {
        // The original exception matters more; a failed rollback leaves the
        // connection marked broken or in-transaction, so the pool drops it.
        log_->warn("ROLLBACK failed: {}", e.what());
    } catch (...) {
        log_->warn("ROLLBACK failed with a non-standard exception");
    }
}

}  // namespace caelitus::db
