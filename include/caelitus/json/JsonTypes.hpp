#pragma once

/// @file
/// nlohmann::json support for project types.
/// @ingroup json
///
/// Include this instead of `<nlohmann/json.hpp>` wherever these types are
/// (de)serialized:
///
/// | C++ type           | JSON                                                              |
/// |--------------------|-------------------------------------------------------------------|
/// | Timestamp          | `"2026-10-02T21:47:03.123456Z"` (RFC 3339; offsets accepted on input) |
/// | Date               | `"2026-10-02"`                                                    |
/// | `std::optional<T>` | T, or `null`                                                      |
///
/// @code
/// Json j = {{"takenAt", nowUtc()}, {"day", Date::todayUtc()}};
/// auto ts = j.at("takenAt").get<caelitus::Timestamp>();
/// @endcode

#include "caelitus/core/DateTime.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace caelitus {
/// The JSON value type used throughout the project.
using Json = nlohmann::json;
}  // namespace caelitus

/// @cond INTERNAL
namespace nlohmann {

template <>
struct adl_serializer<caelitus::Timestamp> {
    static void to_json(json& j, const caelitus::Timestamp& ts) { j = caelitus::toIsoString(ts); }

    static void from_json(const json& j, caelitus::Timestamp& ts) {
        if (!j.is_string()) throw json::type_error::create(302, "timestamp must be an ISO 8601 string", &j);
        try {
            ts = caelitus::parseIsoTimestamp(j.get_ref<const std::string&>());
        } catch (const std::invalid_argument& e) {
            throw json::other_error::create(501, e.what(), &j);
        }
    }
};

template <>
struct adl_serializer<caelitus::Date> {
    static void to_json(json& j, const caelitus::Date& d) { j = d.toString(); }

    // Date has no default constructor, so use the value-returning form.
    static caelitus::Date from_json(const json& j) {
        if (!j.is_string()) throw json::type_error::create(302, "date must be a \"YYYY-MM-DD\" string", &j);
        try {
            return caelitus::Date::parse(j.get_ref<const std::string&>());
        } catch (const std::invalid_argument& e) {
            throw json::other_error::create(501, e.what(), &j);
        }
    }
};

// nlohmann/json 3.11 has no std::optional support of its own.
#if NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR < 12
template <typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json& j, const std::optional<T>& v) {
        if (v) j = *v;
        else j = nullptr;
    }

    static std::optional<T> from_json(const json& j) {
        if (j.is_null()) return std::nullopt;
        return j.template get<T>();
    }
};
#endif

}  // namespace nlohmann
/// @endcond
