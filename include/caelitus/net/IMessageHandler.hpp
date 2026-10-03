#pragma once

/// @file
/// What the TCP server calls for each message.
/// @ingroup net

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace caelitus::net {

/// The client a message came from.
struct ConnectionInfo {
    std::uint64_t id = 0;          ///< Unique per server run; appears in log lines.
    std::string remoteAddress;     ///< Client IP address.
    std::uint16_t remotePort = 0;  ///< Client TCP port.
};

/// Turns one request message into one reply.
///
/// The server knows nothing about the content (JSON, its envelope, methods):
/// that is the handler's job (see api::JsonRpcHandler).
///
/// Called concurrently from worker threads, so implementations must be
/// thread-safe. Requests on one connection are handled one at a time, in
/// order, so replies always come back in request order.
class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;

    /// Handles one request.
    ///
    /// Must not throw: the server cannot build a reply on its own, so an
    /// exception is logged and the connection closed.
    /// @param request     The message, without its terminating `\0`.
    /// @param connection  Who sent it.
    /// @return The reply to send (the server appends the `\0`), or
    ///         std::nullopt to send none (e.g. a JSON-RPC notification).
    virtual std::optional<std::string> handle(std::string_view request, const ConnectionInfo& connection) = 0;

    /// The client sent something unreadable (e.g. an oversized message).
    ///
    /// Runs on an I/O thread: keep it trivial.
    /// @return A reply to send before the connection is closed, or std::nullopt.
    virtual std::optional<std::string> onProtocolError(std::string_view reason, const ConnectionInfo& connection) {
        (void)reason;
        (void)connection;
        return std::nullopt;
    }
};

}  // namespace caelitus::net
