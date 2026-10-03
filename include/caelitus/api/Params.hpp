#pragma once

/// @file
/// Typed access to request parameters.
/// @ingroup api

#include "caelitus/json/JsonTypes.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace caelitus::api {

/// A parameter is missing or has the wrong type/shape (JSON-RPC -32602).
class InvalidParams : public std::runtime_error {
public:
    /// @param field    The parameter, e.g. "authorIds[2]".
    /// @param message  What is wrong; the full message becomes "field: message".
    InvalidParams(std::string field, const std::string& message)
        : std::runtime_error(field + ": " + message),
          field_(std::move(field)) {}
    /// The offending parameter.
    const std::string& field() const noexcept { return field_; }

private:
    std::string field_;
};

/// Typed access to a request's named parameters.
///
/// Only checks JSON shape (types, presence); business rules stay in the
/// services. `null` counts as absent.
///
/// Supported T: std::string, bool, int, std::int64_t, double, Date, Timestamp,
/// `std::vector<std::string>`, `std::vector<std::int64_t>`.
///
/// @code
/// auto id = p.required<std::int64_t>("id");
/// auto title = p.optional<std::string>("titleContains");
/// int page = p.value<int>("page", 1);
/// @endcode
class Params {
public:
    /// @param object  The request's "params"; must outlive this object.
    explicit Params(const Json& object) : json_(object) {}

    /// The parameter, converted to T.
    /// @throws InvalidParams if it is missing, null or of the wrong type.
    template <typename T>
    T required(const char* name) const {
        auto v = optional<T>(name);
        if (!v) throw InvalidParams(name, "is required");
        return std::move(*v);
    }

    /// The parameter converted to T, or std::nullopt if missing or null.
    /// @throws InvalidParams if it has the wrong type.
    template <typename T>
    std::optional<T> optional(const char* name) const {
        auto it = json_.find(name);
        if (it == json_.end() || it->is_null()) return std::nullopt;
        return convert<T>(*it, name);
    }

    /// The parameter converted to T, or `fallback` if missing or null.
    /// @throws InvalidParams if it has the wrong type.
    template <typename T>
    T value(const char* name, T fallback) const {
        auto v = optional<T>(name);
        return v ? std::move(*v) : std::move(fallback);
    }

private:
    template <typename T>
    static T convert(const Json& v, const std::string& name) {
        if constexpr (std::is_same_v<T, std::string>) {
            if (!v.is_string()) throw InvalidParams(name, "must be a string");
            return v.get<std::string>();
        } else if constexpr (std::is_same_v<T, bool>) {
            if (!v.is_boolean()) throw InvalidParams(name, "must be true or false");
            return v.get<bool>();
        } else if constexpr (std::is_integral_v<T>) {
            if (!v.is_number_integer()) throw InvalidParams(name, "must be an integer");
            if (v.is_number_unsigned() &&
                v.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
                throw InvalidParams(name, "is too large");
            const auto i = v.get<std::int64_t>();
            if (i < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                i > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
                throw InvalidParams(name, "is out of range");
            return static_cast<T>(i);
        } else if constexpr (std::is_same_v<T, double>) {
            if (!v.is_number()) throw InvalidParams(name, "must be a number");
            return v.get<double>();
        } else if constexpr (std::is_same_v<T, Date>) {
            try {
                return v.get<Date>();
            } catch (const Json::exception&) {
                throw InvalidParams(name, "must be a date \"YYYY-MM-DD\"");
            }
        } else if constexpr (std::is_same_v<T, Timestamp>) {
            try {
                return v.get<Timestamp>();
            } catch (const Json::exception&) {
                throw InvalidParams(name, "must be an RFC 3339 timestamp");
            }
        } else if constexpr (std::is_same_v<T, std::vector<std::string>> ||
                             std::is_same_v<T, std::vector<std::int64_t>>) {
            if (!v.is_array()) throw InvalidParams(name, "must be an array");
            T out;
            for (std::size_t i = 0; i < v.size(); ++i)
                out.push_back(convert<typename T::value_type>(v[i], name + "[" + std::to_string(i) + "]"));
            return out;
        } else {
            static_assert(sizeof(T) == 0, "Params: unsupported type");
        }
    }

    const Json& json_;
};

}  // namespace caelitus::api
