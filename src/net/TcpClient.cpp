/// @file
/// net::TcpClient on POSIX sockets.
/// @ingroup net

#include "caelitus/net/TcpClient.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fcntl.h>

#include <cerrno>
#include <cstring>

namespace caelitus::net {

namespace {

std::string systemError() { return std::strerror(errno); }

// Waits until fd is ready for `events`; false on timeout.
bool waitFor(int fd, short events, std::chrono::milliseconds timeout) {
    pollfd p{fd, events, 0};
    for (;;) {
        const int n = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (n > 0) return true;
        if (n == 0) return false;
        if (errno != EINTR) throw NetError("poll failed: " + systemError());
    }
}

// Non-blocking connect bounded by timeout; returns the connected socket or -1
// with `error` set.
int connectTo(const addrinfo& a, std::chrono::milliseconds timeout, std::string& error) {
    const int fd = ::socket(a.ai_family, a.ai_socktype | SOCK_CLOEXEC, a.ai_protocol);
    if (fd < 0) return error = systemError(), -1;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, a.ai_addr, a.ai_addrlen) != 0) {
        if (errno != EINPROGRESS) return error = systemError(), ::close(fd), -1;
        if (!waitFor(fd, POLLOUT, timeout)) return error = "timed out", ::close(fd), -1;
        int soError = 0;
        socklen_t len = sizeof soError;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        if (soError != 0) return error = std::strerror(soError), ::close(fd), -1;
    }
    return fd;  // stays non-blocking; reads and writes poll first
}

}  // namespace

TcpClient::TcpClient(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout)
    : timeout_(timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string where = host + ":" + std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found); rc != 0)
        throw NetError("Cannot resolve " + host + ": " + ::gai_strerror(rc));

    std::string error = "no address";
    for (const addrinfo* a = found; a && fd_ < 0; a = a->ai_next) fd_ = connectTo(*a, timeout, error);
    ::freeaddrinfo(found);
    if (fd_ < 0) throw NetError("Cannot connect to " + where + ": " + error);
}

TcpClient::~TcpClient() {
    if (fd_ >= 0) ::close(fd_);
}

void TcpClient::sendAll(std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n > 0) {
            data.remove_prefix(static_cast<std::size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!waitFor(fd_, POLLOUT, timeout_)) throw NetError("Timed out sending the request");
        } else if (n < 0 && errno != EINTR) {
            throw NetError("Sending failed: " + systemError());
        }
    }
}

std::string TcpClient::request(std::string_view message) {
    std::string framed(message);
    framed.push_back('\0');
    sendAll(framed);

    for (;;) {
        if (const auto end = buffer_.find('\0'); end != std::string::npos) {
            std::string reply = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            return reply;
        }
        if (!waitFor(fd_, POLLIN, timeout_))
            throw NetError("No reply within " + std::to_string(timeout_.count()) + " ms");
        char chunk[16384];
        const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
        if (n == 0) throw NetError("The server closed the connection");
        if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            throw NetError("Receiving failed: " + systemError());
        if (n > 0) buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

}  // namespace caelitus::net
