#pragma once

/// @file
/// SQL text helpers (private to the db module).
/// @ingroup db

#include <string>
#include <string_view>

namespace caelitus::db::detail {

/// SQL text for logs and error messages: truncated to `maxLength`, never
/// with parameter values (they may be personal data).
inline std::string shortenSql(std::string_view sql, std::size_t maxLength = 200) {
    if (sql.size() <= maxLength) return std::string(sql);
    std::string out(sql.substr(0, maxLength));
    out += "...";
    return out;
}

}  // namespace caelitus::db::detail
