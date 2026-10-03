#pragma once

/// @file
/// A blocking client for the TCP server's `\0`-framed messages.
/// @ingroup net

#include "caelitus/net/TcpServer.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace caelitus::net {

/// Connects to a TcpServer and exchanges `\0`-terminated messages, one at a
/// time, blocking. Meant for tools (the `--cli` mode), not for the server.
///
/// @code
/// TcpClient client("127.0.0.1", 9000);
/// std::string reply = client.request(R"({"jsonrpc":"2.0","id":1,"method":"system.ping"})");
/// @endcode
///
/// Every operation is bounded by the timeout: a server that does not answer
/// makes request() throw instead of hanging.
class TcpClient {
public:
    /// Resolves `host` (a name or an IPv4/IPv6 address) and connects.
    /// @throws NetError if the name does not resolve, the connection is
    ///         refused, or nothing answers within `timeout`.
    TcpClient(const std::string& host, std::uint16_t port,
              std::chrono::milliseconds timeout = std::chrono::seconds(10));

    /// Closes the connection.
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    /// Sends `message` (a `\0` is appended) and returns the next reply,
    /// without its `\0`.
    /// @throws NetError if the server closes the connection or does not reply
    ///         within the timeout.
    std::string request(std::string_view message);

private:
    void sendAll(std::string_view data);

    int fd_ = -1;
    std::chrono::milliseconds timeout_;
    std::string buffer_;  // received bytes after the last reply
};

}  // namespace caelitus::net
