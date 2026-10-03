#pragma once

/// @file
/// JSON-RPC 2.0 request handling and method registration.
/// @ingroup api

#include "caelitus/api/Params.hpp"
#include "caelitus/json/JsonTypes.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"
#include "caelitus/net/IMessageHandler.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace caelitus::api {

/// JSON-RPC 2.0 error codes.
///
/// -32700..-32600 are the standard protocol errors; -32001.. are ours. Domain
/// errors carry their stable code in `error.data.code` ("isbn_taken",
/// "version_conflict", ...).
namespace errors {
constexpr int kParseError = -32700;      ///< The message is not JSON.
constexpr int kInvalidRequest = -32600;  ///< JSON, but not a JSON-RPC 2.0 request (or a bad batch).
constexpr int kMethodNotFound = -32601;  ///< No such method.
constexpr int kInvalidParams = -32602;   ///< Shape errors and ValidationError.
constexpr int kInternalError = -32603;   ///< A bug or a permanent database error; details are logged, never sent.
constexpr int kNotFound = -32001;        ///< NotFoundError.
constexpr int kConflict = -32002;        ///< ConflictError.
constexpr int kUnavailable = -32003;     ///< Transient database trouble: retry later.
}  // namespace errors

/// A named parameter, described by a JSON Schema (see schema::).
struct Param {
    std::string name;         ///< As it appears in "params".
    Json schema;              ///< What values are accepted; enforced by SchemaValidator.
    bool required = false;    ///< Must be present and not null.
    std::string description;  ///< For the OpenRPC document.
};

/// One API method: what it accepts, returns and may fail with, and what it does.
///
/// The description feeds both request validation and the OpenRPC document
/// (rpc.discover, docs/openrpc.json), so the documentation cannot drift from
/// the code. Usually built with MethodBuilder.
struct Method {
    std::string name;         ///< "books.search".
    std::string tag;          ///< Group in the docs: "books", "reviews", ...
    std::string summary;      ///< One line.
    std::string description;  ///< Longer explanation; may be empty.
    /// Accepted parameters. Unknown names, missing required ones and values
    /// that do not match the schema are rejected before `run` is called.
    std::vector<Param> params;
    std::string resultName;  ///< Name of the result in the docs, e.g. "book".
    Json result;             ///< Schema of the result.
    /// Error components this method can return besides InvalidParams and
    /// Unavailable (which every method can): "NotFound", "Conflict".
    std::vector<std::string> errors;
    /// `data.code` values a Conflict error can carry ("isbn_taken", ...).
    std::vector<std::string> conflicts;
    /// The implementation: validated params in, result out. Throws
    /// DomainError subclasses for expected failures.
    std::function<Json(const Params&)> run;
};

/// Dispatches JSON-RPC 2.0 requests to registered methods; the
/// net::IMessageHandler of the TCP server.
///
/// Requests may be single or batched (at most 100); notifications (no "id")
/// get no reply. Parameters must be named (an object); they are checked
/// against each Param's schema before the method runs. Methods must be
/// registered before the server starts (dispatch is then lock-free).
///
/// Exceptions from a method become error responses:
/// | Exception                                | code   | error.data |
/// |------------------------------------------|--------|------------|
/// | InvalidParams                            | -32602 | field, reason |
/// | ValidationError                          | -32602 | code "validation_failed", field |
/// | NotFoundError                            | -32001 | code "not_found", entity, id |
/// | ConflictError                            | -32002 | code ("isbn_taken", ...) |
/// | db::DatabaseError, transient             | -32003 | none (logged) |
/// | any other exception                      | -32603 | none (logged) |
///
/// @code
/// --> {"jsonrpc":"2.0","id":1,"method":"books.get","params":{"id":42}}
/// <-- {"jsonrpc":"2.0","id":1,"result":{"id":42,"title":"Dune",...}}
/// <-- {"jsonrpc":"2.0","id":1,"error":{"code":-32001,"message":"book 42 not found",
///                                     "data":{"code":"not_found","entity":"book","id":42}}}
/// @endcode
///
/// Logs to "api".
class JsonRpcHandler final : public net::IMessageHandler {
public:
    JsonRpcHandler();

    /// Registers a method.
    /// @throws std::invalid_argument for a method without a name or body, or a duplicate name.
    void add(Method method);
    /// Registered methods, by name.
    const std::map<std::string, Method>& methods() const noexcept { return methods_; }

    /// Registers a named schema that methods refer to with schema::ref("Book").
    /// @throws std::invalid_argument for a duplicate name.
    void addSchema(const std::string& name, Json schema);
    /// Registered schemas, by name.
    const std::map<std::string, Json>& schemas() const noexcept { return schemas_; }

    /// Handles one message: a request or a batch. Never throws.
    std::optional<std::string> handle(std::string_view request, const net::ConnectionInfo& connection) override;
    /// Replies with a JSON-RPC error (id null) before the server disconnects.
    std::optional<std::string> onProtocolError(std::string_view reason, const net::ConnectionInfo& connection) override;

    /// Builds an error response object.
    /// @param id       The request id (null if unknown).
    /// @param code     One of the errors:: codes.
    /// @param message  Short human-readable description.
    /// @param data     Extra details; omitted when null.
    static Json errorResponse(const Json& id, int code, const std::string& message, const Json& data = nullptr);

private:
    std::optional<Json> handleOne(const Json& request, const net::ConnectionInfo& connection);
    Json invoke(const Method& method, const Json& params, const Json& id, const net::ConnectionInfo& connection);

    std::map<std::string, Method> methods_;
    std::map<std::string, Json> schemas_;
    log::Logger log_;
    log::LogThrottle internalLog_, unavailableLog_;
};

/// Readable method registration.
///
/// @code
/// MethodBuilder(rpc, "books.get", "books", "One book with all its details.")
///     .required("id", schema::id(), "Book id")
///     .returns("book", schema::ref("Book"))
///     .errors({"NotFound"})
///     .handler([](const Params& p) { ... });
/// @endcode
class MethodBuilder {
public:
    /// Starts a method; nothing is registered until handler().
    MethodBuilder(JsonRpcHandler& rpc, std::string name, std::string tag, std::string summary) : rpc_(rpc) {
        m_.name = std::move(name);
        m_.tag = std::move(tag);
        m_.summary = std::move(summary);
        m_.result = Json::object();
    }
    /// Sets the longer explanation.
    MethodBuilder& description(std::string text) {
        m_.description = std::move(text);
        return *this;
    }
    /// Adds a required parameter.
    MethodBuilder& required(std::string name, Json schema, std::string description) {
        m_.params.push_back({std::move(name), std::move(schema), true, std::move(description)});
        return *this;
    }
    /// Adds an optional parameter.
    MethodBuilder& optional(std::string name, Json schema, std::string description) {
        m_.params.push_back({std::move(name), std::move(schema), false, std::move(description)});
        return *this;
    }
    /// Describes the result.
    MethodBuilder& returns(std::string name, Json schema) {
        m_.resultName = std::move(name);
        m_.result = std::move(schema);
        return *this;
    }
    /// Lists the error components the method can return ("NotFound", ...).
    MethodBuilder& errors(std::vector<std::string> components) {
        m_.errors = std::move(components);
        return *this;
    }
    /// Lists the Conflict codes the method can return; adds the Conflict error.
    MethodBuilder& conflicts(std::vector<std::string> codes) {
        m_.conflicts = std::move(codes);
        if (std::find(m_.errors.begin(), m_.errors.end(), "Conflict") == m_.errors.end())
            m_.errors.push_back("Conflict");
        return *this;
    }
    /// Sets the implementation and registers the method.
    void handler(std::function<Json(const Params&)> run) {
        m_.run = std::move(run);
        rpc_.add(std::move(m_));
    }

private:
    JsonRpcHandler& rpc_;
    Method m_;
};

}  // namespace caelitus::api
