#pragma once

/// @file
/// Small builders for the JSON Schemas (draft-07 subset) used in the API's
/// OpenRPC description and for request validation.
/// @ingroup api
///
/// See SchemaValidator for the keywords that are enforced.
/// @code
/// namespace S = api::schema;
/// S::object({{"id", S::id()}, {"title", S::string(1, 300)}, {"isbn", S::nullable(S::string())}})
/// @endcode

#include "caelitus/json/JsonTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace caelitus::api::schema {

/// `s` with a "description".
inline Json describe(Json s, const std::string& description) {
    s["description"] = description;
    return s;
}

/// A string, optionally with a length range (in characters).
inline Json string(std::size_t minLength = 0, std::optional<std::size_t> maxLength = std::nullopt) {
    Json s = {{"type", "string"}};
    if (minLength > 0) s["minLength"] = minLength;
    if (maxLength) s["maxLength"] = *maxLength;
    return s;
}

/// An integer, optionally with an inclusive range.
inline Json integer(std::optional<std::int64_t> minimum = std::nullopt,
                    std::optional<std::int64_t> maximum = std::nullopt) {
    Json s = {{"type", "integer"}};
    if (minimum) s["minimum"] = *minimum;
    if (maximum) s["maximum"] = *maximum;
    return s;
}

/// A number, optionally with an inclusive range.
inline Json number(std::optional<double> minimum = std::nullopt, std::optional<double> maximum = std::nullopt) {
    Json s = {{"type", "number"}};
    if (minimum) s["minimum"] = *minimum;
    if (maximum) s["maximum"] = *maximum;
    return s;
}

/// true or false.
inline Json boolean() { return {{"type", "boolean"}}; }
/// A positive integer id.
inline Json id() { return describe(integer(1), "Positive integer id"); }
/// A date string, "2026-10-02".
inline Json date() { return {{"type", "string"}, {"format", "date"}}; }
/// An RFC 3339 timestamp string; results are always UTC ("...Z").
inline Json dateTime() { return {{"type", "string"}, {"format", "date-time"}}; }

/// One of the given strings.
inline Json enumOf(const std::vector<std::string>& values) { return {{"type", "string"}, {"enum", values}}; }
/// Exactly `value`.
inline Json constant(Json value) { return {{"const", std::move(value)}}; }

/// An array of `items`, optionally with a size range.
inline Json array(Json items, std::optional<std::size_t> maxItems = std::nullopt, std::size_t minItems = 0) {
    Json s = {{"type", "array"}, {"items", std::move(items)}};
    if (minItems > 0) s["minItems"] = minItems;
    if (maxItems) s["maxItems"] = *maxItems;
    return s;
}

/// A reference to a schema registered with JsonRpcHandler::addSchema().
inline Json ref(const std::string& name) { return {{"$ref", "#/components/schemas/" + name}}; }

/// A value that may also be null (absent optional fields are returned as null).
inline Json nullable(Json s) { return {{"oneOf", {std::move(s), {{"type", "null"}}}}}; }

/// Properties of an object schema, in display order.
using Properties = std::vector<std::pair<std::string, Json>>;

/// A closed object: every listed property is required unless named in
/// `optional`, and no other property is allowed.
inline Json object(const Properties& properties, const std::vector<std::string>& optional = {}) {
    Json props = Json::object();
    Json required = Json::array();
    for (const auto& [name, s] : properties) {
        props[name] = s;
        if (std::find(optional.begin(), optional.end(), name) == optional.end()) required.push_back(name);
    }
    return {{"type", "object"}, {"properties", props}, {"required", required}, {"additionalProperties", false}};
}

}  // namespace caelitus::api::schema
