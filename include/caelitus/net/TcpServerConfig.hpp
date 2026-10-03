#pragma once

/// @file
/// TCP server settings (the "server" configuration section).
/// @ingroup net

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace caelitus::net {

/// Address, threads and limits of a TcpServer.
struct TcpServerConfig {
    std::string bindAddress = "0.0.0.0";  ///< Interface to listen on; "127.0.0.1" for local clients only.
    std::uint16_t port = 0;               ///< TCP port; 0 picks any free port (tests).

    /// Threads doing socket I/O only. 1-2 handle thousands of connections.
    std::size_t ioThreads = 2;
    /// Threads running IMessageHandler::handle(); size it for blocking work
    /// (database calls), e.g. close to the database pool size.
    std::size_t workerThreads = 8;

    /// Connections beyond this are accepted and closed at once.
    std::size_t maxConnections = 10000;
    /// A message (without its terminating `\0`) larger than this is a protocol
    /// error: the client gets onProtocolError()'s reply and is disconnected.
    std::size_t maxMessageBytes = 1024 * 1024;
    /// Requests a client may send ahead before we stop reading from it
    /// (backpressure).
    std::size_t maxPendingRequests = 16;

    /// A connection with no traffic for this long is closed.
    std::chrono::seconds idleTimeout{300};
    /// Requests taking at least this long are logged as warnings.
    std::chrono::milliseconds slowRequestThreshold{500};
    /// stop() waits this long for in-flight requests before closing forcibly.
    std::chrono::seconds shutdownTimeout{10};
};

}  // namespace caelitus::net
