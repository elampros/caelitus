#include "caelitus/db/mariadb/MariaDbConnection.hpp"

#include "caelitus/db/DbErrors.hpp"
#include "caelitus/db/detail/SqlDateTime.hpp"
#include "db/detail/SqlText.hpp"
#include "db/mariadb/MariaDbErrorTranslator.hpp"

#include <mariadb/conncpp.hpp>

#include <type_traits>
#include <variant>

namespace caelitus::db::mariadb {

namespace {

sql::SQLString toSqlString(std::string_view s) { return sql::SQLString(s.data(), s.size()); }

std::string toStdString(const sql::SQLString& s) { return std::string(s.c_str(), s.length()); }

// SQL text (never parameter values) is included in error messages.
std::string describe(std::string_view action, std::string_view sql) {
    return std::string(action) + " [" + detail::shortenSql(sql) + "]";
}

void bind(sql::PreparedStatement& stmt, const Params& params) {
    int32_t index = 1;
    for (const DbValue& param : params) {
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, DbValue::Null>) stmt.setNull(index, sql::DataType::SQLNULL);
                else if constexpr (std::is_same_v<T, bool>) stmt.setBoolean(index, v);
                else if constexpr (std::is_same_v<T, std::int64_t>) stmt.setInt64(index, v);
                else if constexpr (std::is_same_v<T, std::uint64_t>) stmt.setUInt64(index, v);
                else if constexpr (std::is_same_v<T, double>) stmt.setDouble(index, v);
                else if constexpr (std::is_same_v<T, std::string>) stmt.setString(index, toSqlString(v));
                else if constexpr (std::is_same_v<T, Timestamp>)
                    stmt.setDateTime(index, toSqlString(detail::formatSqlDateTime(v)));
                else if constexpr (std::is_same_v<T, Date>)
                    stmt.setDateTime(index, toSqlString(detail::formatSqlDate(v)));
                else static_assert(sizeof(T) == 0, "unhandled DbValue alternative");
            },
            param.storage());
        ++index;
    }
}

}  // namespace

// ---- MariaDbConnection -------------------------------------------------------

MariaDbConnection::MariaDbConnection(std::unique_ptr<sql::Connection> conn) : conn_(std::move(conn)) {
    if (!conn_) throw std::invalid_argument("MariaDbConnection: connection is null");
}

MariaDbConnection::~MariaDbConnection() {
    try {
        conn_->close();
    } catch (...) {
    }
}

template <typename F>
auto MariaDbConnection::guarded(std::string_view context, F&& f) {
    if (broken_) throw ConnectionError(std::string(context) + ": connection is broken");
    try {
        return f();
    } catch (sql::SQLException& e) {
        const int code = e.getErrorCode();
        const std::string state = e.getSQLStateCStr();
        if (isConnectionFailure(code, state)) broken_ = true;
        throwTranslated(code, state, context, e.what());
    } catch (const DatabaseError&) {
        throw;
    } catch (const std::exception& e) {
        throw DatabaseError(std::string(context) + ": " + e.what());
    }
}

ExecResult MariaDbConnection::execute(std::string_view sqlText, const Params& params) {
    return guarded(describe("Execute failed", sqlText), [&] {
        std::unique_ptr<sql::PreparedStatement> stmt(conn_->prepareStatement(toSqlString(sqlText)));
        bind(*stmt, params);
        ExecResult result;
        result.affectedRows = static_cast<std::uint64_t>(stmt->executeLargeUpdate());
        return result;
    });
}

ExecResult MariaDbConnection::insert(std::string_view sqlText, const Params& params) {
    return guarded(describe("Insert failed", sqlText), [&] {
        std::unique_ptr<sql::PreparedStatement> stmt(
            conn_->prepareStatement(toSqlString(sqlText), sql::Statement::RETURN_GENERATED_KEYS));
        bind(*stmt, params);

        ExecResult result;
        result.affectedRows = static_cast<std::uint64_t>(stmt->executeLargeUpdate());

        std::unique_ptr<sql::ResultSet> keys(stmt->getGeneratedKeys());
        if (keys && keys->next()) result.lastInsertId = keys->getUInt64(1);
        return result;
    });
}

ResultSet MariaDbConnection::query(std::string_view sqlText, const Params& params) {
    return guarded(describe("Query failed", sqlText), [&] {
        std::unique_ptr<sql::PreparedStatement> stmt(conn_->prepareStatement(toSqlString(sqlText)));
        bind(*stmt, params);
        std::unique_ptr<sql::ResultSet> rs(stmt->executeQuery());

        std::unique_ptr<sql::ResultSetMetaData> meta(rs->getMetaData());
        const uint32_t columnCount = meta->getColumnCount();
        std::vector<std::string> names;
        names.reserve(columnCount);
        for (uint32_t i = 1; i <= columnCount; ++i) names.push_back(toStdString(meta->getColumnLabel(i)));
        auto columns = std::make_shared<const ColumnSet>(std::move(names));

        ResultSet rows;
        while (rs->next()) {
            std::vector<Row::Cell> cells;
            cells.reserve(columnCount);
            for (uint32_t i = 1; i <= columnCount; ++i) {
                sql::SQLString value = rs->getString(static_cast<int32_t>(i));
                if (rs->wasNull()) cells.emplace_back(std::nullopt);
                else cells.emplace_back(toStdString(value));
            }
            rows.emplace_back(columns, std::move(cells));
        }
        return rows;
    });
}

void MariaDbConnection::executeRaw(std::string_view sqlText) {
    guarded(describe("Statement failed", sqlText), [&] {
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        stmt->execute(toSqlString(sqlText));
    });
}

void MariaDbConnection::begin(const TransactionOptions& options) {
    if (inTransaction_) throw TransactionError("begin(): a transaction is already open on this connection");
    // SET TRANSACTION (without SESSION) applies to the next transaction only.
    if (options.isolation != IsolationLevel::Default)
        executeRaw(std::string("SET TRANSACTION ISOLATION LEVEL ") + toString(options.isolation));
    executeRaw(options.readOnly ? "START TRANSACTION READ ONLY" : "START TRANSACTION");
    inTransaction_ = true;
}

void MariaDbConnection::commit() {
    if (!inTransaction_) throw TransactionError("commit(): no open transaction");
    executeRaw("COMMIT");
    inTransaction_ = false;
}

void MariaDbConnection::rollback() {
    if (!inTransaction_) return;
    executeRaw("ROLLBACK");
    inTransaction_ = false;
}

bool MariaDbConnection::ping() noexcept {
    if (broken_) return false;
    bool ok = false;
    try {
        ok = conn_->isValid();
    } catch (...) {
    }
    if (!ok) broken_ = true;
    return ok;
}

// ---- MariaDbConnectionFactory ------------------------------------------------

MariaDbConnectionFactory::MariaDbConnectionFactory(DbConfig config) : config_(std::move(config)) {}

std::unique_ptr<IConnection> MariaDbConnectionFactory::create() {
    const std::string target = config_.host + ":" + std::to_string(config_.port) + "/" + config_.database;

    sql::Properties props;
    props["user"] = toSqlString(config_.user);
    props["password"] = toSqlString(config_.password);
    props["connectTimeout"] = toSqlString(std::to_string(config_.connectTimeout.count()));
    props["socketTimeout"] = toSqlString(std::to_string(config_.socketTimeout.count()));
    props["autoReconnect"] = "false";  // the pool replaces broken connections

    std::unique_ptr<sql::Connection> raw;
    try {
        raw.reset(sql::mariadb::get_driver_instance()->connect(toSqlString("jdbc:mariadb://" + target), props));
    } catch (sql::SQLException& e) {
        const int code = e.getErrorCode();
        const std::string state = e.getSQLStateCStr();
        const std::string context = "Cannot connect to " + target;
        if (isConnectionFailure(code, state)) throwTranslated(code, state, context, e.what());
        throw ConnectionError(context + ": " + e.what(), code, state);
    }
    if (!raw) throw ConnectionError("Cannot connect to " + target + ": driver returned no connection");

    auto conn = std::make_unique<MariaDbConnection>(std::move(raw));
    conn->executeRaw("SET NAMES utf8mb4");
    if (!config_.sessionTimeZone.empty()) {
        if (config_.sessionTimeZone.find_first_of("'\\") != std::string::npos)
            throw ConnectionError("Invalid sessionTimeZone '" + config_.sessionTimeZone + "'", 0, {}, false);
        conn->executeRaw("SET time_zone = '" + config_.sessionTimeZone + "'");
    }
    for (const auto& statement : config_.initStatements) conn->executeRaw(statement);
    return conn;
}

}  // namespace caelitus::db::mariadb
