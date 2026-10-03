#pragma once

/// @file
/// MariaDB error numbers to db::DatabaseError subclasses (private to db_mariadb).
/// @ingroup db_mariadb

#include <string>
#include <string_view>

namespace caelitus::db::mariadb {

/// Throws the DatabaseError subclass matching a MariaDB error code / SQLSTATE.
///
/// Kept free of driver headers so it can be unit-tested on its own.
/// @param code           MariaDB error number, e.g. 1062.
/// @param sqlState       SQLSTATE, e.g. "23000".
/// @param context        What was being done, for the message ("query", "commit", ...).
/// @param driverMessage  The connector's message.
[[noreturn]] void throwTranslated(int code, const std::string& sqlState, std::string_view context,
                                  std::string_view driverMessage);

/// True if the error leaves the connection unusable (lost, closed, timed out).
bool isConnectionFailure(int code, const std::string& sqlState) noexcept;

}  // namespace caelitus::db::mariadb
