#pragma once

/// @file
/// Query results: rows, column names and typed access.
/// @ingroup db

#include "caelitus/db/DbErrors.hpp"
#include "caelitus/db/detail/SqlDateTime.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace caelitus::db {

/// @cond INTERNAL
namespace detail {
template <typename T>
struct IsSystemTimePoint : std::false_type {};
template <typename Duration>
struct IsSystemTimePoint<std::chrono::time_point<std::chrono::system_clock, Duration>> : std::true_type {};
}  // namespace detail
/// @endcond

/// Column names of a result, shared by all rows of that result.
class ColumnSet {
public:
    /// @param names  Column names in result order.
    explicit ColumnSet(std::vector<std::string> names) : names_(std::move(names)) {
        for (std::size_t i = 0; i < names_.size(); ++i) index_.emplace(names_[i], i);
    }

    std::size_t size() const noexcept { return names_.size(); }            ///< Number of columns.
    const std::string& name(std::size_t i) const { return names_.at(i); }  ///< Name of column `i`.

    /// Position of the named column.
    /// @throws DataMappingError if there is no such column.
    std::size_t indexOf(std::string_view name) const {
        auto it = index_.find(std::string(name));
        if (it == index_.end()) throw DataMappingError("Unknown column '" + std::string(name) + "'");
        return it->second;
    }

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, std::size_t> index_;
};

/// One fully materialized result row.
///
/// Values are kept in their textual form (as the server sends them) and
/// converted on access, so the row is independent of the driver and safe to
/// keep after the connection has been returned to the pool.
///
/// Supported `get<T>`: std::string, bool, integral and floating-point types,
/// Date (DATE) and Timestamp or any system_clock time point (DATETIME,
/// TIMESTAMP; read as UTC, so the session time zone must be UTC).
///
/// @code
/// for (const Row& r : sql.query("SELECT id, title, page_count FROM books")) {
///     auto id = r.get<std::int64_t>("id");
///     auto pages = r.getOptional<int>("page_count");  // NULL -> nullopt
/// }
/// @endcode
class Row {
public:
    using Cell = std::optional<std::string>;  ///< A value in text form; std::nullopt is SQL NULL.

    /// Drivers build rows; application code only reads them.
    Row(std::shared_ptr<const ColumnSet> columns, std::vector<Cell> cells)
        : columns_(std::move(columns)),
          cells_(std::move(cells)) {}

    std::size_t size() const noexcept { return cells_.size(); }      ///< Number of columns.
    const ColumnSet& columns() const noexcept { return *columns_; }  ///< The result's column names.

    /// True if the value at `index` is SQL NULL.
    bool isNull(std::size_t index) const { return !cell(index).has_value(); }
    /// True if the named column is SQL NULL.
    bool isNull(std::string_view column) const { return isNull(columns_->indexOf(column)); }

    /// The value at `index`, converted to T.
    /// @throws DataMappingError if the value is NULL or cannot be converted.
    template <typename T>
    T get(std::size_t index) const {
        const Cell& c = cell(index);
        if (!c) throw DataMappingError("Column '" + columns_->name(index) + "' is NULL");
        return convert<T>(*c, index);
    }

    /// The value of the named column, converted to T.
    /// @throws DataMappingError if there is no such column, or the value is
    ///         NULL or cannot be converted.
    template <typename T>
    T get(std::string_view column) const {
        return get<T>(columns_->indexOf(column));
    }

    /// The value at `index` converted to T, or std::nullopt for NULL.
    /// @throws DataMappingError if the value cannot be converted.
    template <typename T>
    std::optional<T> getOptional(std::size_t index) const {
        const Cell& c = cell(index);
        if (!c) return std::nullopt;
        return convert<T>(*c, index);
    }

    /// The value of the named column converted to T, or std::nullopt for NULL.
    /// @throws DataMappingError if there is no such column or the value cannot be converted.
    template <typename T>
    std::optional<T> getOptional(std::string_view column) const {
        return getOptional<T>(columns_->indexOf(column));
    }

private:
    const Cell& cell(std::size_t index) const {
        if (index >= cells_.size()) throw DataMappingError("Column index " + std::to_string(index) + " out of range");
        return cells_[index];
    }

    template <typename T>
    T convert(const std::string& text, std::size_t index) const {
        if constexpr (std::is_same_v<T, std::string>) {
            return text;
        } else if constexpr (std::is_same_v<T, Date>) {
            try {
                return detail::parseSqlDate(text);
            } catch (const std::invalid_argument& e) {
                failWith(e.what(), index);
            }
        } else if constexpr (detail::IsSystemTimePoint<T>::value) {
            Timestamp ts;
            try {
                ts = detail::parseSqlDateTime(text);
            } catch (const std::invalid_argument& e) {
                failWith(e.what(), index);
            }
            using D = typename T::duration;
            if constexpr (std::ratio_less_v<typename D::period, std::micro>) {
                // e.g. nanosecond system_clock only reaches 1677..2262
                const auto us = ts.time_since_epoch();
                if (us > std::chrono::duration_cast<std::chrono::microseconds>(D::max()) ||
                    us < std::chrono::duration_cast<std::chrono::microseconds>(D::min()))
                    failWith("value out of range for the requested time_point type", index);
            }
            return std::chrono::floor<D>(ts);
        } else if constexpr (std::is_same_v<T, bool>) {
            if (text == "1" || text == "true" || text == "TRUE") return true;
            if (text == "0" || text == "false" || text == "FALSE") return false;
            fail<T>(text, index);
        } else if constexpr (std::is_arithmetic_v<T>) {
            T value{};
            const char* end = text.data() + text.size();
            auto [ptr, ec] = std::from_chars(text.data(), end, value);
            if (ec != std::errc{} || ptr != end) fail<T>(text, index);
            return value;
        } else {
            static_assert(sizeof(T) == 0, "Row::get<T>: unsupported type");
        }
    }

    template <typename T>
    [[noreturn]] void fail(const std::string& text, std::size_t index) const {
        throw DataMappingError("Cannot convert value '" + text + "' of column '" + columns_->name(index) +
                               "' to the requested type");
    }

    [[noreturn]] void failWith(const std::string& reason, std::size_t index) const {
        throw DataMappingError("Column '" + columns_->name(index) + "': " + reason);
    }

    std::shared_ptr<const ColumnSet> columns_;
    std::vector<Cell> cells_;
};

/// All rows of a query result.
using ResultSet = std::vector<Row>;

/// Outcome of an INSERT / UPDATE / DELETE.
struct ExecResult {
    std::uint64_t affectedRows = 0;  ///< Rows inserted, changed or deleted.
    std::uint64_t lastInsertId = 0;  ///< Generated AUTO_INCREMENT id; set by insert() only.
};

}  // namespace caelitus::db
