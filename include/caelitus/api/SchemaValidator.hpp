#pragma once

/// @file
/// Validation of JSON values against the API's schemas.
/// @ingroup api

#include "caelitus/json/JsonTypes.hpp"

#include <map>
#include <optional>
#include <string>

namespace caelitus::api {

/// Validates JSON values against the JSON Schema subset the API uses.
///
/// Enforced keywords: `type` (incl. "integer"), `enum`, `const`, `minimum`,
/// `maximum`, `minLength` / `maxLength` (in characters), `format` "date" /
/// "date-time", `items`, `minItems`, `maxItems`, `uniqueItems`, `properties`,
/// `required`, `additionalProperties: false`, `oneOf`, and `$ref` to
/// "#/components/schemas/<Name>".
///
/// Other keywords (description, title, examples, ...) are ignored. An unknown
/// `$ref` is reported as an error (a bug in the API description).
class SchemaValidator {
public:
    /// @param components  Named schemas for `$ref`; must outlive the validator.
    explicit SchemaValidator(const std::map<std::string, Json>& components) : components_(components) {}

    /// Checks `value` against `schema`.
    /// @param value   The JSON to check.
    /// @param schema  The schema it must match.
    /// @param path  Name of the value in messages, e.g. "authorIds".
    /// @return std::nullopt if valid, else "<path>: <problem>" for the first
    ///         problem found (e.g. "authorIds[1]: must be at least 1").
    std::optional<std::string> check(const Json& value, const Json& schema, const std::string& path) const;

private:
    const std::map<std::string, Json>& components_;
};

}  // namespace caelitus::api
