// Tests for TcpServer over real loopback sockets.

#include "LogCapture.hpp"
#include "TestHarness.hpp"

#include "caelitus/log/Log.hpp"
#include "caelitus/net/TcpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace caelitus::net;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::shared_ptr<test::CaptureSink> g_logs = std::make_shared<test::CaptureSink>();

// Blocking POSIX client with a receive timeout, so a broken server fails a
// test instead of hanging it.
class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
            throw test::Failure{std::string("connect failed: ") + std::strerror(errno)};
        timeval tv{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~Client() {
        if (fd_ >= 0) ::close(fd_);
    }

    void send(std::string_view raw) {
        while (!raw.empty()) {
            const ssize_t n = ::send(fd_, raw.data(), raw.size(), MSG_NOSIGNAL);
            if (n <= 0) throw test::Failure{"send failed"};
            raw.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    void request(std::string_view message) {
        std::string m(message);
        m.push_back('\0');
        send(m);
    }
    void shutdownWrite() { ::shutdown(fd_, SHUT_WR); }

    // Next \0-terminated reply; nullopt if the server closed the connection.
    std::optional<std::string> reply() {
        for (;;) {
            const auto pos = buf_.find('\0');
            if (pos != std::string::npos) {
                std::string out = buf_.substr(0, pos);
                buf_.erase(0, pos + 1);
                return out;
            }
            char chunk[4096];
            const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
            if (n == 0) return std::nullopt;
            if (n < 0) throw test::Failure{"timed out waiting for a reply"};
            buf_.append(chunk, static_cast<std::size_t>(n));
        }
    }

    std::string expectReply() {
        auto r = reply();
        if (!r) throw test::Failure{"connection closed instead of a reply"};
        return *r;
    }

    // True once the server has closed the connection (within the timeout).
    bool closedByServer() {
        if (!buf_.empty()) return false;
        char c;
        for (;;) {
            const ssize_t n = ::recv(fd_, &c, 1, 0);
            if (n == 0) return true;
            if (n < 0) return errno == ECONNRESET;
        }
    }

private:
    int fd_ = -1;
    std::string buf_;
};

// Commands understood by the test handler:
//   "throw"     -> handler throws
//   "none"      -> no reply
//   "sleep:N"   -> sleeps N ms, then echoes
//   anything    -> "echo:<request>"
class TestHandler final : public IMessageHandler {
public:
    std::optional<std::string> handle(std::string_view request, const ConnectionInfo&) override {
        const int now = ++running;
        int prev = maxRunning.load();
        while (now > prev && !maxRunning.compare_exchange_weak(prev, now)) {
        }
        struct Done {
            std::atomic<int>& r;
            ~Done() { --r; }
        } done{running};

        if (request == "throw") throw std::runtime_error("boom");
        if (request == "none") return std::nullopt;
        if (request.rfind("sleep:", 0) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(std::string(request.substr(6)))));
        return "echo:" + std::string(request);
    }

    std::optional<std::string> onProtocolError(std::string_view reason, const ConnectionInfo&) override {
        return R"({"error":")" + std::string(reason) + R"("})";
    }

    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
};

TcpServerConfig baseConfig() {
    TcpServerConfig c;
    c.bindAddress = "127.0.0.1";
    c.port = 0;
    c.ioThreads = 2;
    c.workerThreads = 4;
    c.shutdownTimeout = 5s;
    return c;
}

struct Fixture {
    std::shared_ptr<TestHandler> handler = std::make_shared<TestHandler>();
    TcpServer server;

    explicit Fixture(TcpServerConfig config = baseConfig()) : server(config, handler) {
        g_logs->clear();
        server.start();
    }
    Client client() { return Client(server.port()); }
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = Clock::now() + timeout;
    while (!pred()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

}  // namespace

// ---- Framing -----------------------------------------------------------------

TEST(request_and_reply) {
    Fixture f;
    Client c = f.client();
    c.request(R"({"method":"ping"})");
    CHECK_EQ(c.expectReply(), R"(echo:{"method":"ping"})");
}

TEST(several_messages_in_one_packet) {
    Fixture f;
    Client c = f.client();
    c.send(std::string("a\0b\0c\0", 6));
    CHECK_EQ(c.expectReply(), "echo:a");
    CHECK_EQ(c.expectReply(), "echo:b");
    CHECK_EQ(c.expectReply(), "echo:c");
}

TEST(message_split_across_packets) {
    Fixture f;
    Client c = f.client();
    const std::string msg = "{\"a\":\"θερμοκρασία\"}";
    for (char ch : msg) {
        c.send(std::string_view(&ch, 1));
        std::this_thread::sleep_for(1ms);
    }
    c.send(std::string_view("\0", 1));
    CHECK_EQ(c.expectReply(), "echo:" + msg);
}

TEST(empty_messages_are_ignored) {
    Fixture f;
    Client c = f.client();
    c.send(std::string("\0\0x\0", 4));
    CHECK_EQ(c.expectReply(), "echo:x");
}

TEST(final_message_without_terminator_is_answered_on_half_close) {
    Fixture f;
    Client c = f.client();
    c.send("first");
    c.send(std::string_view("\0", 1));
    c.send("last-without-terminator");
    c.shutdownWrite();
    CHECK_EQ(c.expectReply(), "echo:first");
    CHECK_EQ(c.expectReply(), "echo:last-without-terminator");
    CHECK(c.closedByServer());
}

TEST(handler_may_send_no_reply) {
    Fixture f;
    Client c = f.client();
    c.request("none");
    c.request("ping");
    CHECK_EQ(c.expectReply(), "echo:ping");
}

// ---- Ordering and concurrency ------------------------------------------------

TEST(replies_keep_request_order) {
    Fixture f;
    Client c = f.client();
    c.send(std::string("sleep:150\0fast\0", 15));
    CHECK_EQ(c.expectReply(), "echo:sleep:150");
    CHECK_EQ(c.expectReply(), "echo:fast");
}

TEST(slow_handler_does_not_block_other_connections) {
    Fixture f;
    Client slow = f.client();
    Client fast = f.client();
    slow.request("sleep:800");
    std::this_thread::sleep_for(50ms);
    const auto t0 = Clock::now();
    fast.request("ping");
    CHECK_EQ(fast.expectReply(), "echo:ping");
    CHECK(Clock::now() - t0 < 300ms);
    CHECK_EQ(slow.expectReply(), "echo:sleep:800");
}

TEST(connections_are_served_in_parallel_by_workers) {
    Fixture f;  // 4 workers
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    const auto t0 = Clock::now();
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            Client c(f.server.port());
            c.request("sleep:300");
            if (c.reply() == std::optional<std::string>("echo:sleep:300")) ++ok;
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(ok.load(), 4);
    CHECK(Clock::now() - t0 < 900ms);  // ~300 ms in parallel, 1200 ms serially
    CHECK_EQ(f.handler->maxRunning.load(), 4);
}

TEST(pipelined_requests_are_all_answered_with_backpressure) {
    TcpServerConfig cfg = baseConfig();
    cfg.maxPendingRequests = 2;
    Fixture f(cfg);
    Client c = f.client();
    std::string burst;
    for (int i = 0; i < 200; ++i) burst += "m" + std::to_string(i) + std::string(1, '\0');
    c.send(burst);
    for (int i = 0; i < 200; ++i) CHECK_EQ(c.expectReply(), "echo:m" + std::to_string(i));
}

TEST(many_clients_many_requests) {
    TcpServerConfig cfg = baseConfig();
    cfg.workerThreads = 8;
    Fixture f(cfg);
    constexpr int kClients = 50, kRequests = 200;
    std::atomic<int> good{0};
    std::vector<std::thread> threads;
    const auto t0 = Clock::now();
    for (int i = 0; i < kClients; ++i)
        threads.emplace_back([&, i] {
            Client c(f.server.port());
            for (int r = 0; r < kRequests; ++r) {
                const std::string msg = std::to_string(i) + "/" + std::to_string(r);
                c.request(msg);
                if (c.reply() == std::optional<std::string>("echo:" + msg)) ++good;
            }
        });
    for (auto& t : threads) t.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK_EQ(good.load(), kClients * kRequests);
    CHECK_EQ(f.server.stats().requests, static_cast<std::uint64_t>(kClients * kRequests));
    std::cout << "       " << kClients * kRequests << " request/reply round trips in " << secs * 1000 << " ms ("
              << static_cast<int>(kClients * kRequests / secs) << " req/s)\n";
}

// ---- Limits and errors -------------------------------------------------------

TEST(oversized_message_gets_error_reply_and_is_closed) {
    TcpServerConfig cfg = baseConfig();
    cfg.maxMessageBytes = 64;
    Fixture f(cfg);
    Client c = f.client();
    c.send(std::string(100, 'x'));  // no terminator, already too big
    CHECK_EQ(c.expectReply(), R"({"error":"message exceeds 64 bytes"})");
    CHECK(c.closedByServer());
    CHECK_EQ(f.server.stats().protocolErrors, 1u);
    CHECK(g_logs->contains(spdlog::level::warn, "message exceeds 64 bytes"));

    Client c2 = f.client();  // terminated but too big
    c2.request(std::string(65, 'y'));
    CHECK_EQ(c2.expectReply(), R"({"error":"message exceeds 64 bytes"})");
    CHECK(c2.closedByServer());
}

TEST(handler_exception_closes_connection_but_server_survives) {
    Fixture f;
    Client bad = f.client();
    bad.request("throw");
    CHECK(bad.closedByServer());
    CHECK_EQ(f.server.stats().handlerErrors, 1u);
    CHECK(g_logs->contains(spdlog::level::err, "Handler threw on connection"));

    Client good = f.client();
    good.request("ok");
    CHECK_EQ(good.expectReply(), "echo:ok");
}

TEST(idle_connections_are_closed) {
    TcpServerConfig cfg = baseConfig();
    cfg.idleTimeout = 1s;
    Fixture f(cfg);
    Client c = f.client();
    const auto t0 = Clock::now();
    CHECK(c.closedByServer());
    const auto elapsed = Clock::now() - t0;
    CHECK(elapsed > 800ms && elapsed < 3s);
}

TEST(long_request_is_not_idle) {
    TcpServerConfig cfg = baseConfig();
    cfg.idleTimeout = 1s;
    Fixture f(cfg);
    Client c = f.client();
    c.request("sleep:1500");
    CHECK_EQ(c.expectReply(), "echo:sleep:1500");
}

TEST(connection_limit_rejects_extra_clients) {
    TcpServerConfig cfg = baseConfig();
    cfg.maxConnections = 2;
    Fixture f(cfg);
    Client a = f.client();
    Client b = f.client();
    a.request("x");
    b.request("y");
    CHECK_EQ(a.expectReply(), "echo:x");
    CHECK_EQ(b.expectReply(), "echo:y");

    Client c = f.client();  // TCP accepts, server closes at once
    CHECK(c.closedByServer());
    CHECK_EQ(f.server.stats().rejectedConnections, 1u);
}

TEST(slow_requests_are_warned) {
    TcpServerConfig cfg = baseConfig();
    cfg.slowRequestThreshold = 50ms;
    Fixture f(cfg);
    Client c = f.client();
    c.request("sleep:80");
    c.expectReply();
    CHECK(waitFor([] { return g_logs->contains(spdlog::level::warn, "Slow request"); }));
    CHECK(g_logs->contains(spdlog::level::warn, "sleep:80"));
}

// ---- Lifecycle ---------------------------------------------------------------

TEST(stop_lets_in_flight_requests_finish) {
    auto f = std::make_unique<Fixture>();
    Client c = f->client();
    c.request("sleep:300");
    std::this_thread::sleep_for(50ms);
    std::thread stopper([&] { f->server.stop(); });
    CHECK_EQ(c.expectReply(), "echo:sleep:300");
    CHECK(c.closedByServer());
    stopper.join();
}

TEST(stop_with_idle_clients_is_quick) {
    Fixture f;
    Client a = f.client();
    Client b = f.client();
    a.request("x");
    a.expectReply();
    const auto t0 = Clock::now();
    f.server.stop();
    CHECK(Clock::now() - t0 < 1s);
    CHECK(a.closedByServer());
    CHECK(b.closedByServer());
}

TEST(port_in_use_is_an_error) {
    Fixture f;
    TcpServerConfig cfg = baseConfig();
    cfg.port = f.server.port();
    TcpServer second(cfg, std::make_shared<TestHandler>());
    CHECK_THROWS_AS(second.start(), NetError);
}

int main() {
    caelitus::log::LogConfig config;
    config.level = "debug";
    config.console = false;
    config.extraSinks = {g_logs};
    caelitus::log::init(config);
    return test::runAll();
}
