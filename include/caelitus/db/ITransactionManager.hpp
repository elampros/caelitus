#pragma once

/// @file
/// The transaction interface services depend on.
/// @ingroup db

#include "caelitus/db/TransactionOptions.hpp"

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace caelitus::db {

/// What services depend on to group repository calls atomically.
///
/// It says nothing about which database (or whether any database) is behind
/// it, so a service can be unit-tested with a trivial fake.
///
/// @code
/// auto id = tx.inTransaction([&] {
///     auto id = orders.insert(order);
///     stock.decrement(order.itemId, order.qty);
///     return id;
/// });
///
/// auto report = tx.inTransaction(TransactionOptions::readOnlyTx(), [&] { ... });
/// tx.inTransaction({IsolationLevel::Serializable}, [&] { ... });
/// @endcode
///
/// Rules for the work:
/// - It may run more than once (transient failures such as deadlocks are
///   retried), so it must not have side effects outside the database. Publish
///   events after inTransaction() returns.
/// - An exception leaving it rolls the transaction back and propagates.
/// - Calls nested on the same thread join the outer transaction. If nested work
///   fails, the outer transaction is rolled back even if the caller catches the
///   exception. A nested call asking for a different explicit isolation level
///   throws TransactionError, since the level cannot change once a
///   transaction has started.
class ITransactionManager {
public:
    virtual ~ITransactionManager() = default;

    /// Runs `work` in a transaction with default options and returns its result.
    template <typename F>
    auto inTransaction(F&& work) -> std::invoke_result_t<F&> {
        return inTransaction(TransactionOptions{}, std::forward<F>(work));
    }

    /// Runs `work` in a transaction with the given options and returns its result.
    template <typename F>
    auto inTransaction(const TransactionOptions& options, F&& work) -> std::invoke_result_t<F&> {
        using R = std::invoke_result_t<F&>;
        if constexpr (std::is_void_v<R>) {
            run(options, [&] { work(); });
        } else {
            std::optional<R> result;
            run(options, [&] { result.emplace(work()); });
            return std::move(*result);
        }
    }

protected:
    /// Implemented by each transaction manager: run `work` in a transaction.
    virtual void run(const TransactionOptions& options, const std::function<void()>& work) = 0;
};

}  // namespace caelitus::db
