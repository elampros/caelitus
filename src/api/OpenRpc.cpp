#include "caelitus/api/OpenRpc.hpp"

#include "caelitus/api/Schema.hpp"

#include <set>

namespace caelitus::api {

namespace {

Json errorComponents() {
    return {
        {"InvalidParams",
         {{"code", errors::kInvalidParams},
          {"message", "Invalid params"},
          {"data",
           {{"field", "the offending parameter"},
            {"reason", "what is wrong with it"},
            {"code", "\"validation_failed\" when a business rule rejected the value"}}}}},
        {"NotFound",
         {{"code", errors::kNotFound},
          {"message", "Not found"},
          {"data",
           {{"code", "not_found"},
            {"entity", "book, author, ..., job"},
            {"id", "the id (or, for jobs, the name) that was not found"}}}}},
        {"Conflict",
         {{"code", errors::kConflict},
          {"message", "Conflict"},
          {"data", {{"code", "machine-readable reason; listed per method"}}}}},
        {"Unavailable", {{"code", errors::kUnavailable}, {"message", "Service temporarily unavailable; retry later"}}},
    };
}

const char* kTransport =
    "JSON-RPC 2.0 over TCP. Every message, request or reply, is a JSON document terminated by a NUL byte "
    "(\\0). Requests on one connection are answered in order; batches (arrays of up to 100 requests) and "
    "notifications (requests without an id) are supported. Parameters are always passed by name.\n\n"
    "Protocol errors: -32700 parse error, -32600 invalid request, -32601 method not found, -32603 internal "
    "error (details are logged on the server, never returned). Application errors: see components.errors; "
    "error.data.code is a stable machine-readable reason.\n\n"
    "Conventions: ids are positive integers; dates are \"YYYY-MM-DD\"; timestamps are RFC 3339 in UTC; "
    "absent optional values are null; lists are paged with page (from 1) and pageSize (1-100).";

}  // namespace

Json openRpcDocument(const JsonRpcHandler& rpc, const ApiInfo& info) {
    Json methods = Json::array();
    std::set<std::string> tags;
    for (const auto& [name, m] : rpc.methods()) {
        Json params = Json::array();
        for (const auto& p : m.params) {
            Json param = {{"name", p.name}, {"required", p.required}, {"schema", p.schema}};
            if (!p.description.empty()) param["description"] = p.description;
            params.push_back(std::move(param));
        }

        Json methodErrors = Json::array();
        auto addError = [&](const std::string& component) {
            methodErrors.push_back({{"$ref", "#/components/errors/" + component}});
        };
        if (!m.params.empty()) addError("InvalidParams");
        for (const auto& e : m.errors) addError(e);
        addError("Unavailable");

        std::string description = m.description;
        if (!m.conflicts.empty()) {
            description += std::string(description.empty() ? "" : "\n\n") + "Conflict error data.code values: ";
            for (std::size_t i = 0; i < m.conflicts.size(); ++i) description += (i ? ", " : "") + m.conflicts[i];
            description += ".";
        }

        Json method = {{"name", name},
                       {"summary", m.summary},
                       {"paramStructure", "by-name"},
                       {"params", params},
                       {"result", {{"name", m.resultName}, {"schema", m.result}}},
                       {"errors", methodErrors}};
        if (!description.empty()) method["description"] = description;
        if (!m.tag.empty()) {
            method["tags"] = Json::array({{{"$ref", "#/components/tags/" + m.tag}}});
            tags.insert(m.tag);
        }
        methods.push_back(std::move(method));
    }

    Json tagComponents = Json::object();
    for (const auto& t : tags) tagComponents[t] = {{"name", t}};

    Json schemas = Json::object();
    for (const auto& [name, s] : rpc.schemas()) schemas[name] = s;

    return {{"openrpc", "1.3.2"},
            {"info",
             {{"title", info.title},
              {"version", info.version},
              {"description", info.description + (info.description.empty() ? "" : "\n\n") + kTransport}}},
            {"servers",
             Json::array({{{"name", "default"},
                           {"url", "tcp://{host}:{port}"},
                           {"summary", "The caelitus server (see server.port in config.json)"},
                           {"variables", {{"host", {{"default", "localhost"}}}, {"port", {{"default", "9000"}}}}}}})},
            {"methods", methods},
            {"components", {{"schemas", schemas}, {"errors", errorComponents()}, {"tags", tagComponents}}}};
}

void addDiscover(JsonRpcHandler& rpc, ApiInfo info) {
    MethodBuilder(rpc, "rpc.discover", "system", "Returns this API's OpenRPC description.")
        .description(
            "The full machine-readable API description (https://spec.open-rpc.org): methods, parameters, "
            "results, errors and schemas. Paste it into https://playground.open-rpc.org to browse it.")
        .returns("openrpcDocument", {{"type", "object"}, {"description", "An OpenRPC 1.3 document"}})
        .handler([&rpc, info](const Params&) { return openRpcDocument(rpc, info); });
}

}  // namespace caelitus::api
