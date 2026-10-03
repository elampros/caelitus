#pragma once

/// @file
/// Which transaction is open on this thread (private to the db module).
/// @ingroup db

#include "caelitus/db/IConnection.hpp"

namespace caelitus::db::detail {

/// The transaction currently open on this thread for a given pool.
///
/// Lets SqlExecutor route statements to the transaction's connection without
/// the service or repository passing it around. Stored in a thread_local.
struct TransactionBinding {
    const void* owner = nullptr;  ///< The ConnectionPool.
    IConnection* conn = nullptr;  ///< The transaction's connection.
    TransactionOptions options;   ///< How it was started.
    bool rollbackOnly = false;    ///< A failure occurred; the transaction must not commit...
    bool retryable = false;       ///< ...and that failure was transient.
};

/// The binding for `owner` on this thread; nullptr when no transaction is open.
TransactionBinding* currentTransaction(const void* owner) noexcept;

/// Binds a connection as the current transaction for the guard's lifetime.
class BindingGuard {
public:
    BindingGuard(const void* owner, IConnection* conn, const TransactionOptions& options);
    ~BindingGuard();
    BindingGuard(const BindingGuard&) = delete;
    BindingGuard& operator=(const BindingGuard&) = delete;

    TransactionBinding& binding() noexcept;

private:
    const void* owner_;
};

}  // namespace caelitus::db::detail
