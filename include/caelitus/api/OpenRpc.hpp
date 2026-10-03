#pragma once

/// @file
/// The OpenRPC description of the API.
/// @ingroup api

#include "caelitus/api/JsonRpc.hpp"

#include <string>

namespace caelitus::api {

/// The "info" section of an OpenRPC document.
struct ApiInfo {
    std::string title;        ///< API name.
    std::string version;      ///< API version (the build version).
    std::string description;  ///< Overview, Markdown.
};

/// The OpenRPC 1.3 document (https://spec.open-rpc.org) for every method and
/// schema registered on `rpc`.
///
/// Committed as docs/openrpc.json (`cmake --build <dir> --target openrpc`);
/// paste it into https://playground.open-rpc.org to browse the API.
Json openRpcDocument(const JsonRpcHandler& rpc, const ApiInfo& info);

/// Registers "rpc.discover", which returns openRpcDocument().
///
/// Register it after the other methods; the document is built on each call,
/// from the (by then immutable) registry.
void addDiscover(JsonRpcHandler& rpc, ApiInfo info);

}  // namespace caelitus::api
