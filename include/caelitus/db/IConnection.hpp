#pragma once

/// @file
/// The driver interface: one database connection.
/// @ingroup db

#include "caelitus/db/DbValue.hpp"
#include "caelitus/db/Row.hpp"
#include "caelitus/db/TransactionOptions.hpp"

#include <memory>
#include <string_view>

namespace caelitus::db {

/// A single physical database connection.
///
/// Implemented once per driver (see mariadb::MariaDbConnection); the rest of
/// the db layer only uses this interface. All methods throw only DatabaseError
/// subclasses. Not thread-safe: a connection is used by one thread at a time,
/// which ConnectionPool guarantees.
class IConnection {
public:
    virtual ~IConnection() = default;

    /// Runs a statement with `?` placeholders bound positionally to `params`.
    /// @return The number of affected rows.
    virtual ExecResult execute(std::string_view sql, const Params& params) = 0;
    /// Like execute(), also returning the AUTO_INCREMENT id it generated.
    ///
    /// Meant for single-row INSERTs: the driver derives the ids of every
    /// inserted row, which is very slow for large multi-row INSERTs.
    virtual ExecResult insert(std::string_view sql, const Params& params) = 0;
    /// Runs a SELECT and reads the whole result into memory.
    virtual ResultSet query(std::string_view sql, const Params& params) = 0;

    /// Starts a transaction.
    virtual void begin(const TransactionOptions& options) = 0;
    /// Commits the current transaction.
    virtual void commit() = 0;
    /// Rolls the current transaction back.
    virtual void rollback() = 0;
    /// True between begin() and commit() / rollback().
    virtual bool inTransaction() const noexcept = 0;

    /// Round-trips to the server; false if the connection is unusable.
    virtual bool ping() noexcept = 0;

    /// Set once a connection-level failure has been observed; the pool
    /// discards broken connections instead of reusing them.
    virtual bool isBroken() const noexcept = 0;
};

/// Opens connections for ConnectionPool. One implementation per driver.
class IConnectionFactory {
public:
    virtual ~IConnectionFactory() = default;

    /// Opens a new connection.
    /// @throws ConnectionError if the server cannot be reached or refuses the login.
    virtual std::unique_ptr<IConnection> create() = 0;
};

}  // namespace caelitus::db
