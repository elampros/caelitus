#include "caelitus/api/JsonRpc.hpp"

#include "caelitus/api/SchemaValidator.hpp"

#include "caelitus/core/DomainErrors.hpp"
#include "caelitus/db/DbErrors.hpp"

#include <algorithm>
#include <chrono>

namespace caelitus::api {

namespace {

constexpr std::size_t kMaxBatch = 100;

bool validId(const Json& id) { return id.is_string() || id.is_number() || id.is_null(); }

Json success(const Json& id, Json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

}  // namespace

JsonRpcHandler::JsonRpcHandler() : log_(log::get("api")) {}

void JsonRpcHandler::add(Method method) {
    if (method.name.empty() || !method.run)
        throw std::invalid_argument("JsonRpcHandler: method needs a name and a body");
    const std::string name = method.name;
    if (!methods_.emplace(name, std::move(method)).second)
        throw std::invalid_argument("JsonRpcHandler: method '" + name + "' registered twice");
}

void JsonRpcHandler::addSchema(const std::string& name, Json schema) {
    if (!schemas_.emplace(name, std::move(schema)).second)
        throw std::invalid_argument("JsonRpcHandler: schema '" + name + "' registered twice");
}

Json JsonRpcHandler::errorResponse(const Json& id, int code, const std::string& message, const Json& data) {
    Json error = {{"code", code}, {"message", message}};
    if (!data.is_null()) error["data"] = data;
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
}

std::optional<std::string> JsonRpcHandler::onProtocolError(std::string_view reason, const net::ConnectionInfo&) {
    return errorResponse(nullptr, errors::kInvalidRequest, "Invalid Request", {{"reason", std::string(reason)}}).dump();
}

std::optional<std::string> JsonRpcHandler::handle(std::string_view text, const net::ConnectionInfo& connection) {
    Json request;
    try {
        request = Json::parse(text);
    } catch (const Json::parse_error& e) {
        return errorResponse(nullptr, errors::kParseError, "Parse error", {{"reason", e.what()}}).dump();
    }

    if (!request.is_array()) {
        auto reply = handleOne(request, connection);
        return reply ? std::optional<std::string>(reply->dump()) : std::nullopt;
    }

    if (request.empty() || request.size() > kMaxBatch)
        return errorResponse(nullptr, errors::kInvalidRequest, "Invalid Request",
                             {{"reason", "a batch must hold 1.." + std::to_string(kMaxBatch) + " requests"}})
            .dump();
    Json replies = Json::array();
    for (const auto& item : request)
        if (auto reply = handleOne(item, connection)) replies.push_back(std::move(*reply));
    if (replies.empty()) return std::nullopt;  // only notifications
    return replies.dump();
}

std::optional<Json> JsonRpcHandler::handleOne(const Json& request, const net::ConnectionInfo& connection) {
    auto invalid = [](const Json& id, const std::string& reason) {
        return errorResponse(id, errors::kInvalidRequest, "Invalid Request", {{"reason", reason}});
    };
    if (!request.is_object()) return invalid(nullptr, "a request must be an object");

    const bool notification = !request.contains("id");
    const Json id = notification ? Json(nullptr) : request["id"];
    if (!validId(id)) return invalid(nullptr, "id must be a string, a number or null");
    auto version = request.find("jsonrpc");
    if (version == request.end() || !version->is_string() || *version != "2.0")
        return invalid(id, "jsonrpc must be \"2.0\"");
    if (!request.contains("method") || !request["method"].is_string()) return invalid(id, "method must be a string");

    const Json params = request.contains("params") ? request["params"] : Json::object();

    const std::string name = request["method"].get<std::string>();
    auto it = methods_.find(name);
    Json reply = it == methods_.end()
                     ? errorResponse(id, errors::kMethodNotFound, "Method not found", {{"method", name}})
                     : invoke(it->second, params, id, connection);
    if (notification) return std::nullopt;
    return reply;
}

Json JsonRpcHandler::invoke(const Method& method, const Json& params, const Json& id,
                            const net::ConnectionInfo& connection) {
    if (!params.is_object())
        return errorResponse(id, errors::kInvalidParams, "Invalid params", {{"reason", "params must be an object"}});
    auto invalid = [&](const std::string& field, const std::string& reason) {
        return errorResponse(id, errors::kInvalidParams, "Invalid params", {{"field", field}, {"reason", reason}});
    };
    for (const auto& item : params.items()) {
        const std::string& key = item.key();
        const bool known =
            std::any_of(method.params.begin(), method.params.end(), [&](const Param& p) { return p.name == key; });
        if (!known) {
            Json accepted = Json::array();
            for (const auto& p : method.params) accepted.push_back(p.name);
            return errorResponse(id, errors::kInvalidParams, "Invalid params",
                                 {{"field", key}, {"reason", "unknown parameter"}, {"accepted", accepted}});
        }
    }
    const SchemaValidator validator(schemas_);
    for (const auto& p : method.params) {
        auto it = params.find(p.name);
        if (it == params.end() || it->is_null()) {  // null means "not given"
            if (p.required) return invalid(p.name, p.name + ": is required");
            continue;
        }
        if (auto problem = validator.check(*it, p.schema, p.name)) return invalid(p.name, *problem);
    }

    try {
        return success(id, method.run(Params(params)));
    } catch (const InvalidParams& e) {
        return errorResponse(id, errors::kInvalidParams, "Invalid params",
                             {{"field", e.field()}, {"reason", e.what()}});
    } catch (const ValidationError& e) {
        return errorResponse(id, errors::kInvalidParams, e.what(), {{"code", e.code()}, {"field", e.field()}});
    } catch (const NotFoundError& e) {
        return errorResponse(id, errors::kNotFound, e.what(),
                             {{"code", e.code()}, {"entity", e.entity()}, {"id", e.id()}});
    } catch (const ConflictError& e) {
        return errorResponse(id, errors::kConflict, e.what(), {{"code", e.code()}});
    } catch (const db::DatabaseError& e) {
        if (e.isTransient()) {
            if (auto suppressed = unavailableLog_.allow())
                log_->warn("{} failed (transient, connection #{}): {}{}", method.name, connection.id, e.what(),
                           log::suppressedSuffix(*suppressed));
            return errorResponse(id, errors::kUnavailable, "Service temporarily unavailable; retry later");
        }
        if (auto suppressed = internalLog_.allow())
            log_->error("{} failed (connection #{}): {}{}", method.name, connection.id, e.what(),
                        log::suppressedSuffix(*suppressed));
        return errorResponse(id, errors::kInternalError, "Internal error");
    } catch (const std::exception& e) {
        if (auto suppressed = internalLog_.allow())
            log_->error("{} failed (connection #{}): {}{}", method.name, connection.id, e.what(),
                        log::suppressedSuffix(*suppressed));
        return errorResponse(id, errors::kInternalError, "Internal error");
    }
}

}  // namespace caelitus::api
