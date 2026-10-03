#pragma once

/// @file
/// Helpers for building dynamic SQL safely (private to catalog_mariadb).
/// @ingroup catalog_mariadb

#include "caelitus/db/DbValue.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace caelitus::catalog::mariadb {

/// Collects WHERE conditions and their parameters side by side, so dynamic
/// queries never splice values into SQL text.
///
/// @code
/// SqlFilter f;
/// if (q.language) f.add("b.language = ?", {*q.language});
/// sql.query("SELECT ... FROM books b" + f.where(), f.params());
/// @endcode
class SqlFilter {
public:
    /// Adds a condition with its `?` parameters (ANDed with the others).
    SqlFilter& add(std::string condition, const db::Params& params = {}) {
        conditions_.push_back(std::move(condition));
        params_.insert(params_.end(), params.begin(), params.end());
        return *this;
    }

    /// "" or " WHERE a AND b".
    std::string where() const {
        std::string out;
        for (std::size_t i = 0; i < conditions_.size(); ++i) out += (i == 0 ? " WHERE " : " AND ") + conditions_[i];
        return out;
    }

    /// The parameters of every condition, in order.
    const db::Params& params() const { return params_; }

private:
    std::vector<std::string> conditions_;
    db::Params params_;
};

/// "?, ?, ?" for n parameters (for IN lists).
inline std::string placeholders(std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) out += (i == 0 ? "?" : ", ?");
    return out;
}

/// LIKE pattern matching `text` anywhere, with `%`, `_` and `\` escaped.
inline std::string likeContains(std::string_view text) {
    std::string out = "%";
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('%');
    return out;
}

/// The ids' values as statement parameters.
template <typename IdT>
db::Params idParams(const std::vector<IdT>& ids) {
    db::Params params;
    params.reserve(ids.size());
    for (const auto& id : ids) params.emplace_back(id.value);
    return params;
}

}  // namespace caelitus::catalog::mariadb
