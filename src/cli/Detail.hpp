#pragma once

/// @file
/// Helpers shared by the cli sources.
/// @ingroup cli

#include "caelitus/json/JsonTypes.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace caelitus::cli::detail {

/// `object[key]` when it is an array, else an empty array; a reference, so
/// pointers to its elements stay valid (unlike `value(key, Json::array())`,
/// which returns a copy).
inline const Json& arrayAt(const Json& object, const char* key) {
    static const Json empty = Json::array();
    if (!object.is_object()) return empty;
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? *it : empty;
}

/// Levenshtein distance between `a` and `b`, ignoring ASCII case.
inline std::size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            const bool same = std::tolower(static_cast<unsigned char>(a[i - 1])) ==
                              std::tolower(static_cast<unsigned char>(b[j - 1]));
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (same ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

}  // namespace caelitus::cli::detail
