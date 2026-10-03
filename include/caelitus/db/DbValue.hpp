#pragma once

/// @file
/// Statement parameter values.
/// @ingroup db

#include "caelitus/core/DateTime.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace caelitus::db {

/// A single bound parameter value.
///
/// Wraps a variant with explicit constructors, so that `"text"` binds as a
/// string (not as bool) and every integer width maps to a fixed 64-bit
/// representation. Any system_clock time point binds as a UTC Timestamp,
/// truncated to microseconds; an empty std::optional binds as NULL.
///
/// @code
/// sql.execute("UPDATE books SET title = ?, page_count = ? WHERE id = ?",
///             {title, std::optional<int>{}, id});   // page_count = NULL
/// @endcode
class DbValue {
public:
    using Null = std::monostate;  ///< SQL NULL.
    /// The values a parameter can hold.
    using Storage = std::variant<Null, bool, std::int64_t, std::uint64_t, double, std::string, Timestamp, Date>;

    /// @name Constructors
    /// One per supported C++ type; implicit on purpose, so parameter lists
    /// read like `{title, id}`.
    /// @{
    DbValue() = default;            ///< NULL.
    DbValue(std::nullptr_t) {}      ///< NULL.
    DbValue(bool v) : value_(v) {}  ///< BOOLEAN (TINYINT 0/1).
    /// A string; a null pointer binds as NULL.
    DbValue(const char* v) : value_(v ? Storage{std::string(v)} : Storage{Null{}}) {}
    DbValue(std::string v) : value_(std::move(v)) {}         ///< A string.
    DbValue(std::string_view v) : value_(std::string(v)) {}  ///< A string (copied).
    DbValue(const Date& v) : value_(v) {}                    ///< DATE.

    /// DATETIME: any system_clock time point, as UTC, floored to microseconds.
    template <typename Duration>
    DbValue(std::chrono::time_point<std::chrono::system_clock, Duration> v)
        : value_(std::chrono::floor<std::chrono::microseconds>(v)) {}

    /// Any integer type, stored as a 64-bit signed or unsigned value.
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    DbValue(T v) {
        if constexpr (std::is_signed_v<T>) value_ = static_cast<std::int64_t>(v);
        else value_ = static_cast<std::uint64_t>(v);
    }

    /// Any floating-point type, stored as double.
    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    DbValue(T v) : value_(static_cast<double>(v)) {}

    /// The contained value, or NULL if empty.
    template <typename T>
    DbValue(const std::optional<T>& v) {
        if (v) *this = DbValue(*v);
    }
    /// @}

    /// True for SQL NULL.
    bool isNull() const noexcept { return std::holds_alternative<Null>(value_); }
    /// The value, for drivers to bind.
    const Storage& storage() const noexcept { return value_; }

private:
    Storage value_;
};

/// Parameters of one statement, bound in order to its `?` placeholders.
using Params = std::vector<DbValue>;

}  // namespace caelitus::db
