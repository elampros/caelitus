#pragma once

/// @file
/// The database layer's exceptions.
/// @ingroup db

#include <stdexcept>
#include <string>

namespace caelitus::db {

/// Base of the database-agnostic exception hierarchy.
///
/// Nothing above the db layer ever sees a driver-specific exception (e.g.
/// `sql::SQLException`): every driver error is translated into one of these.
///
/// @verbatim
/// DatabaseError
/// ├── ConnectionError          (could not connect / connection lost)   transient
/// ├── PoolTimeoutError         (no free connection within timeout)     transient
/// ├── TransientError           (deadlock, lock wait timeout)           transient
/// ├── ConstraintViolationError
/// │   ├── DuplicateKeyError
/// │   └── ForeignKeyError
/// ├── QueryError               (syntax, unknown column, bad data, ...)
/// ├── DataMappingError         (missing column, NULL, bad conversion)
/// └── TransactionError         (misuse of the transaction API)
/// @endverbatim
///
/// TransactionManager retries a unit of work when isTransient() is true.
class DatabaseError : public std::runtime_error {
public:
    /// @param message   Human-readable description.
    /// @param code      Vendor error code, 0 if none.
    /// @param sqlState  Five-character SQLSTATE, empty if none.
    explicit DatabaseError(const std::string& message, int code = 0, std::string sqlState = {})
        : std::runtime_error(message),
          code_(code),
          sqlState_(std::move(sqlState)) {}

    /// Vendor error code (0 when not applicable), e.g. 1062 for a MariaDB duplicate key.
    int code() const noexcept { return code_; }
    /// SQLSTATE, e.g. "23000"; empty when not applicable.
    const std::string& sqlState() const noexcept { return sqlState_; }

    /// True when retrying the whole unit of work may succeed.
    virtual bool isTransient() const noexcept { return false; }

private:
    int code_;
    std::string sqlState_;
};

/// Could not connect, or the connection was lost.
class ConnectionError : public DatabaseError {
public:
    /// @param message    Human-readable description.
    /// @param code       Vendor error code, 0 if none.
    /// @param sqlState   SQLSTATE, empty if none.
    /// @param transient  False for failures retrying cannot fix (wrong
    ///                   credentials, unknown database, ...).
    explicit ConnectionError(const std::string& message, int code = 0, std::string sqlState = {}, bool transient = true)
        : DatabaseError(message, code, std::move(sqlState)),
          transient_(transient) {}

    bool isTransient() const noexcept override { return transient_; }

private:
    bool transient_;
};

/// No free connection within PoolConfig::acquireTimeout. Transient.
class PoolTimeoutError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
    bool isTransient() const noexcept override { return true; }
};

/// A failure that retrying the transaction may fix: deadlock, lock wait timeout. Transient.
class TransientError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
    bool isTransient() const noexcept override { return true; }
};

/// A constraint of the schema rejected the statement.
class ConstraintViolationError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
};

/// A UNIQUE or PRIMARY KEY constraint was violated. The message names the key.
class DuplicateKeyError : public ConstraintViolationError {
public:
    using ConstraintViolationError::ConstraintViolationError;
};

/// A FOREIGN KEY constraint was violated: a missing parent row, or a delete of a row still referenced.
class ForeignKeyError : public ConstraintViolationError {
public:
    using ConstraintViolationError::ConstraintViolationError;
};

/// The statement is wrong: syntax error, unknown column, data too long, ...
class QueryError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
};

/// A result could not be read as requested: unknown column, unexpected NULL, bad conversion.
class DataMappingError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
};

/// The transaction API was misused, e.g. a nested transaction with different options.
class TransactionError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
};

}  // namespace caelitus::db
