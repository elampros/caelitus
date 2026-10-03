#pragma once

/// @file
/// db::IConnection on MariaDB Connector/C++.
/// @ingroup db_mariadb

#include "caelitus/db/DbConfig.hpp"
#include "caelitus/db/IConnection.hpp"

#include <memory>
#include <string_view>

/// @cond INTERNAL
namespace sql {
class Connection;
}  // namespace sql
/// @endcond

namespace caelitus::db::mariadb {

/// IConnection over MariaDB Connector/C++.
///
/// Every `sql::SQLException` is translated to a DatabaseError (by MariaDB
/// error number: 1062 DuplicateKeyError, 1213 TransientError, ...);
/// connection-level failures mark the connection broken, so the pool drops it.
class MariaDbConnection final : public IConnection {
public:
    /// Takes ownership of an open connector connection.
    explicit MariaDbConnection(std::unique_ptr<sql::Connection> conn);
    ~MariaDbConnection() override;

    ExecResult execute(std::string_view sql, const Params& params) override;
    ExecResult insert(std::string_view sql, const Params& params) override;
    ResultSet query(std::string_view sql, const Params& params) override;

    void begin(const TransactionOptions& options) override;
    void commit() override;
    void rollback() override;
    bool inTransaction() const noexcept override { return inTransaction_; }

    bool ping() noexcept override;
    bool isBroken() const noexcept override { return broken_; }

    /// Runs a parameterless statement (used for DbConfig::initStatements).
    void executeRaw(std::string_view sql);

private:
    template <typename F>
    auto guarded(std::string_view context, F&& f);

    std::unique_ptr<sql::Connection> conn_;
    bool inTransaction_ = false;
    bool broken_ = false;
};

/// Opens MariaDbConnection instances.
///
/// Each new connection uses utf8mb4, the configured session time zone (UTC by
/// default) and runs DbConfig::initStatements.
class MariaDbConnectionFactory final : public IConnectionFactory {
public:
    /// @param config  Server address, credentials and timeouts.
    explicit MariaDbConnectionFactory(DbConfig config);
    /// @throws ConnectionError (not transient for wrong credentials or an unknown database).
    std::unique_ptr<IConnection> create() override;

private:
    DbConfig config_;
};

}  // namespace caelitus::db::mariadb
