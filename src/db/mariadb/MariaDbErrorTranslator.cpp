#include "db/mariadb/MariaDbErrorTranslator.hpp"

#include "caelitus/db/DbErrors.hpp"

namespace caelitus::db::mariadb {

namespace {

// https://mariadb.com/kb/en/mariadb-error-code-reference/
bool isOneOf(int code, std::initializer_list<int> codes) {
    for (int c : codes)
        if (c == code) return true;
    return false;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool isAuthOrConfigFailure(int code) {
    return isOneOf(code, {1044 /* db access denied */, 1045 /* access denied */, 1049 /* unknown database */,
                          1251 /* auth protocol */});
}

}  // namespace

bool isConnectionFailure(int code, const std::string& sqlState) noexcept {
    return startsWith(sqlState, "08") || isAuthOrConfigFailure(code) ||
           isOneOf(code, {2002, 2003, 2005, 2006, 2013, 2055,  // client: cannot connect / gone away / lost
                          1040,                                // too many connections
                          1053,                                // server shutdown in progress
                          1158, 1159, 1160, 1161,              // network read/write errors / timeouts
                          1927,                                // connection killed
                          4031});                              // disconnected for inactivity
}

void throwTranslated(int code, const std::string& sqlState, std::string_view context, std::string_view driverMessage) {
    std::string msg(context);
    msg += ": ";
    msg += driverMessage;
    msg += " [code " + std::to_string(code);
    if (!sqlState.empty()) msg += ", state " + sqlState;
    msg += "]";

    if (isAuthOrConfigFailure(code)) throw ConnectionError(msg, code, sqlState, /*transient=*/false);
    if (isConnectionFailure(code, sqlState)) throw ConnectionError(msg, code, sqlState);

    if (isOneOf(code, {1213 /* deadlock */, 1205 /* lock wait timeout */, 1020 /* record changed */}) ||
        sqlState == "40001")
        throw TransientError(msg, code, sqlState);

    if (isOneOf(code, {1062, 1586, 1022})) throw DuplicateKeyError(msg, code, sqlState);
    if (isOneOf(code, {1451, 1452, 1216, 1217})) throw ForeignKeyError(msg, code, sqlState);
    if (startsWith(sqlState, "23")) throw ConstraintViolationError(msg, code, sqlState);

    throw QueryError(msg, code, sqlState);
}

}  // namespace caelitus::db::mariadb
