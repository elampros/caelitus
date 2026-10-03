#pragma once

/// @file
/// The TCP server.
/// @ingroup net

#include "caelitus/net/IMessageHandler.hpp"
#include "caelitus/net/TcpServerConfig.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace caelitus::net {

/// The server could not start, e.g. the port is in use.
class NetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Asynchronous TCP server for `\0`-terminated request/response messages.
///
/// - A few I/O threads multiplex all connections (no thread per connection).
/// - Complete messages go to a worker pool, so a handler blocking on the
///   database never stalls other connections' I/O.
/// - Each reply is sent with a trailing `\0`. If a client closes its sending
///   side, a last message without `\0` is still handled and answered.
/// - Limits: max connections, max message size, pipelined requests per
///   connection (backpressure: reading pauses), idle timeout.
///
/// @code
/// TcpServer server(config.server, std::make_shared<api::JsonRpcHandler>());
/// server.start();
/// ...
/// server.stop();
/// @endcode
///
/// Try it by hand: `printf '{"jsonrpc":"2.0","id":1,"method":"rpc.discover"}\0' | nc -q1 127.0.0.1 9000`.
///
/// Logs to "server": start/stop at info; connections at debug; every request
/// at trace; slow requests, protocol errors and limits at warn (throttled);
/// handler exceptions at error (throttled).
class TcpServer {
public:
    /// Counters since start, for monitoring.
    struct Stats {
        std::uint64_t activeConnections = 0;    ///< Open right now.
        std::uint64_t totalConnections = 0;     ///< Accepted since start.
        std::uint64_t rejectedConnections = 0;  ///< Closed at once: maxConnections reached.
        std::uint64_t requests = 0;             ///< Messages handled.
        std::uint64_t handlerErrors = 0;        ///< IMessageHandler::handle() threw.
        std::uint64_t protocolErrors = 0;       ///< Unreadable input, e.g. an oversized message.
    };

    /// @param config   Address, threads and limits.
    /// @param handler  Turns each request into a reply.
    TcpServer(TcpServerConfig config, std::shared_ptr<IMessageHandler> handler);
    /// Stops the server.
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /// Binds and starts accepting. A server can be started once.
    /// @throws NetError if the address cannot be bound (e.g. port in use).
    void start();

    /// Stops accepting, lets in-flight and already received requests finish
    /// (up to TcpServerConfig::shutdownTimeout), then closes everything.
    /// Idempotent.
    void stop();

    /// The bound port (useful with port 0).
    std::uint16_t port() const;

    /// Connection and request counters.
    Stats stats() const;

    struct Impl;  ///< Implementation (Asio types stay out of this header).

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace caelitus::net
