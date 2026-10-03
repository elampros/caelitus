#pragma once

/// @file
/// Conversions between core date types and SQL literals, shared by drivers.
/// @ingroup db

#include "caelitus/core/DateTime.hpp"

#include <string>
#include <string_view>

namespace caelitus::db::detail {

/// "YYYY-MM-DD".
std::string formatSqlDate(const Date& date);
/// "YYYY-MM-DD HH:MM:SS[.ffffff]" (UTC).
std::string formatSqlDateTime(Timestamp ts);

/// Parses "YYYY-MM-DD".
/// @throws std::invalid_argument on malformed input or a MariaDB zero date
///         ("0000-00-00"), which has no valid representation.
Date parseSqlDate(std::string_view text);
/// Parses "YYYY-MM-DD HH:MM:SS[.ffffff]" as UTC; also accepts a bare date (midnight).
/// @throws std::invalid_argument on malformed input or a zero date.
Timestamp parseSqlDateTime(std::string_view text);

}  // namespace caelitus::db::detail
