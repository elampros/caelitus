#pragma once

/// @file
/// Isolation level and access mode of a transaction.
/// @ingroup db

namespace caelitus::db {

/// SQL transaction isolation levels.
enum class IsolationLevel {
    Default,          ///< Whatever the server/session is configured with (InnoDB: REPEATABLE READ).
    ReadUncommitted,  ///< READ UNCOMMITTED.
    ReadCommitted,    ///< READ COMMITTED.
    RepeatableRead,   ///< REPEATABLE READ.
    Serializable,     ///< SERIALIZABLE.
};

/// The SQL name, e.g. "REPEATABLE READ"; "DEFAULT" for IsolationLevel::Default.
inline const char* toString(IsolationLevel level) noexcept {
    switch (level) {
        case IsolationLevel::Default: return "DEFAULT";
        case IsolationLevel::ReadUncommitted: return "READ UNCOMMITTED";
        case IsolationLevel::ReadCommitted: return "READ COMMITTED";
        case IsolationLevel::RepeatableRead: return "REPEATABLE READ";
        case IsolationLevel::Serializable: return "SERIALIZABLE";
    }
    return "?";
}

/// How a transaction is started.
struct TransactionOptions {
    IsolationLevel isolation = IsolationLevel::Default;  ///< Isolation level.
    /// The server rejects writes; also lets InnoDB skip some bookkeeping.
    bool readOnly = false;

    /// A read-only transaction at the default isolation level.
    static TransactionOptions readOnlyTx() { return {IsolationLevel::Default, true}; }
    /// A read-write transaction at the given isolation level.
    static TransactionOptions withIsolation(IsolationLevel level) { return {level, false}; }
};

}  // namespace caelitus::db
