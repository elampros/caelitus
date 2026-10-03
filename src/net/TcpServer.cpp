/// @file
/// net::TcpServer on Asio: accepting, `\0` framing, the worker pool, ordered
/// replies, limits, backpressure, idle timeouts and graceful stop.
/// @ingroup net

#include "caelitus/net/TcpServer.hpp"

#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace caelitus::net {

using asio::ip::tcp;
using Clock = std::chrono::steady_clock;  ///< For timeouts and request durations.

namespace {

constexpr std::size_t kPreviewLength = 200;

std::string preview(std::string_view text) {
    std::string out(text.substr(0, kPreviewLength));
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    if (text.size() > kPreviewLength) out += "...";
    return out;
}

double millis(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

}  // namespace

class Session;

// ---- Server state ------------------------------------------------------------

struct TcpServer::Impl : std::enable_shared_from_this<TcpServer::Impl> {
    Impl(TcpServerConfig c, std::shared_ptr<IMessageHandler> h)
        : config(std::move(c)),
          handler(std::move(h)),
          log(log::get("server")) {}

    void accept();
    void sessionClosed(std::uint64_t id, std::uint64_t served, const std::string& reason);
    std::size_t activeCount() {
        std::lock_guard<std::mutex> lock(sessionsMutex);
        return sessions.size();
    }
    std::vector<std::shared_ptr<Session>> liveSessions() {
        std::vector<std::shared_ptr<Session>> out;
        std::lock_guard<std::mutex> lock(sessionsMutex);
        for (auto& [id, weak] : sessions)
            if (auto s = weak.lock()) out.push_back(std::move(s));
        return out;
    }

    TcpServerConfig config;
    std::shared_ptr<IMessageHandler> handler;
    log::Logger log;

    asio::io_context io;
    tcp::acceptor acceptor{io};
    asio::steady_timer acceptRetry{io};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::vector<std::thread> ioThreads;
    std::unique_ptr<asio::thread_pool> workers;  // declared after io: destroyed first

    std::mutex sessionsMutex;
    std::condition_variable sessionsCv;
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> sessions;
    std::atomic<std::uint64_t> nextId{1};

    std::mutex lifecycleMutex;
    bool started = false;
    bool stopped = false;
    std::atomic<bool> stopping{false};
    std::uint16_t boundPort = 0;

    std::atomic<std::uint64_t> totalConnections{0}, rejectedConnections{0}, requests{0}, handlerErrors{0},
        protocolErrors{0};
    log::LogThrottle slowLog, protocolLog, limitLog, acceptLog, handlerLog;
};

// ---- One connection ----------------------------------------------------------
//
// All members are touched only on the connection's strand (the socket's
// executor), except info_ (immutable) and runHandler(), which runs on a
// worker and posts its result back to the strand.

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<TcpServer::Impl> server, tcp::socket socket, std::uint64_t id)
        : server_(std::move(server)),
          socket_(std::move(socket)),
          idleTimer_(socket_.get_executor()) {
        info_.id = id;
        asio::error_code ec;
        const auto ep = socket_.remote_endpoint(ec);
        if (!ec) {
            info_.remoteAddress = ep.address().to_string();
            info_.remotePort = ep.port();
        }
    }

    const ConnectionInfo& info() const { return info_; }

    void start() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->armIdleTimer();
            self->read();
        });
    }

    // Graceful: finish requests already received, then close.
    void requestClose() {
        asio::post(socket_.get_executor(), [self = shared_from_this()] {
            if (self->closed_) return;
            self->closeRequested_ = true;
            asio::error_code ec;
            self->socket_.shutdown(tcp::socket::shutdown_receive, ec);  // ends a pending read
            self->maybeFinish();
        });
    }

    void forceClose() {
        asio::post(socket_.get_executor(), [self = shared_from_this()] { self->close("server shutdown (forced)"); });
    }

private:
    const TcpServerConfig& config() const { return server_->config; }

    // ---- reading ----

    void read() {
        if (reading_ || closed_ || eof_ || closeRequested_ || protocolFailed_) return;
        if (requests_.size() >= config().maxPendingRequests) return;  // backpressure: resumed by processNext()
        reading_ = true;
        socket_.async_read_some(asio::buffer(readBuf_),
                                [self = shared_from_this()](asio::error_code ec, std::size_t n) {
                                    self->onRead(ec, n);
                                });
    }

    void onRead(asio::error_code ec, std::size_t n) {
        reading_ = false;
        if (closed_) return;
        if (ec == asio::error::eof) {
            eof_ = true;
            // "Usually" \0-terminated: a final message without \0 still counts.
            if (!closeRequested_ && !pending_.empty()) {
                if (pending_.size() > config().maxMessageBytes) return protocolError(tooLarge());
                requests_.push_back(std::move(pending_));
            }
            pending_.clear();
            processNext();
            maybeFinish();
            return;
        }
        if (ec == asio::error::operation_aborted) return;
        if (ec) return close("read error: " + ec.message());

        armIdleTimer();
        const std::size_t scanFrom = pending_.size();
        pending_.append(readBuf_.data(), n);

        std::size_t start = 0;
        for (std::size_t pos = pending_.find('\0', scanFrom); pos != std::string::npos;
             pos = pending_.find('\0', start)) {
            if (pos - start > config().maxMessageBytes) return protocolError(tooLarge());
            if (pos > start) requests_.emplace_back(pending_, start, pos - start);  // skip empty messages
            start = pos + 1;
        }
        pending_.erase(0, start);
        if (pending_.size() > config().maxMessageBytes) return protocolError(tooLarge());

        processNext();
        read();
    }

    std::string tooLarge() const { return "message exceeds " + std::to_string(config().maxMessageBytes) + " bytes"; }

    void protocolError(const std::string& reason) {
        if (protocolFailed_) return;
        protocolFailed_ = true;
        protocolReason_ = "protocol error: " + reason;
        requests_.clear();
        pending_.clear();
        ++server_->protocolErrors;
        if (auto suppressed = server_->protocolLog.allow())
            server_->log->warn("Connection #{} ({}): {}; closing{}", info_.id, info_.remoteAddress, reason,
                               log::suppressedSuffix(*suppressed));
        try {
            errorReply_ = server_->handler->onProtocolError(reason, info_);
        } catch (...) {
            errorReply_.reset();
        }
        flushErrorReply();
        maybeFinish();
    }

    // The error reply goes after the reply to a request already in progress.
    void flushErrorReply() {
        if (busy_ || !errorReply_) return;
        write(std::move(*errorReply_));
        errorReply_.reset();
    }

    // ---- handling ----

    void processNext() {
        if (busy_ || requests_.empty() || closed_ || protocolFailed_) return;
        busy_ = true;
        std::string request = std::move(requests_.front());
        requests_.pop_front();
        read();  // room in the queue again
        asio::post(*server_->workers, [self = shared_from_this(), request = std::move(request)]() mutable {
            self->runHandler(std::move(request));
        });
    }

    // Worker thread.
    void runHandler(std::string request) {
        const auto start = Clock::now();
        std::optional<std::string> reply;
        std::optional<std::string> error;
        try {
            reply = server_->handler->handle(request, info_);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "non-standard exception";
        }
        const auto elapsed = Clock::now() - start;
        const bool slow = elapsed >= server_->config.slowRequestThreshold;
        std::string requestPreview = (slow || error) ? preview(request) : std::string();

        asio::post(socket_.get_executor(),
                   [self = shared_from_this(), reply = std::move(reply), error = std::move(error), elapsed,
                    size = request.size(), requestPreview = std::move(requestPreview), slow]() mutable {
                       self->onHandled(std::move(reply), std::move(error), elapsed, size, requestPreview, slow);
                   });
    }

    void onHandled(std::optional<std::string> reply, std::optional<std::string> error, Clock::duration elapsed,
                   std::size_t requestSize, const std::string& requestPreview, bool slow) {
        busy_ = false;
        ++served_;
        ++server_->requests;
        auto& log = server_->log;

        if (error) {
            ++server_->handlerErrors;
            if (auto suppressed = server_->handlerLog.allow())
                log->error("Handler threw on connection #{}: {}; closing. Request: {}{}", info_.id, *error,
                           requestPreview, log::suppressedSuffix(*suppressed));
            return close("handler error");
        }
        if (slow) {
            if (auto suppressed = server_->slowLog.allow())
                log->warn("Slow request on connection #{}: {:.1f} ms (threshold {} ms): {}{}", info_.id,
                          millis(elapsed), config().slowRequestThreshold.count(), requestPreview,
                          log::suppressedSuffix(*suppressed));
        } else if (log->should_log(spdlog::level::trace)) {
            log->trace("#{} request {} B -> reply {} B in {:.2f} ms", info_.id, requestSize, reply ? reply->size() : 0,
                       millis(elapsed));
        }
        if (closed_) return;

        if (reply) write(std::move(*reply));
        flushErrorReply();
        processNext();
        maybeFinish();
    }

    // ---- writing ----

    void write(std::string message) {
        message.push_back('\0');
        writeQueue_.push_back(std::move(message));
        if (!writing_) doWrite();
    }

    void doWrite() {
        writing_ = true;
        asio::async_write(socket_, asio::buffer(writeQueue_.front()),
                          [self = shared_from_this()](asio::error_code ec, std::size_t) {
                              self->writing_ = false;
                              if (self->closed_) return;
                              if (ec) return self->close("write error: " + ec.message());
                              self->writeQueue_.pop_front();
                              self->armIdleTimer();
                              if (!self->writeQueue_.empty()) self->doWrite();
                              else self->maybeFinish();
                          });
    }

    // ---- lifecycle ----

    void maybeFinish() {
        if (closed_) return;
        const bool drained = !busy_ && requests_.empty() && writeQueue_.empty() && !writing_ && !errorReply_;
        if (!drained) return;
        if (protocolFailed_) close(protocolReason_);
        else if (eof_) close("client closed the connection");
        else if (closeRequested_) close("server shutdown");
    }

    void armIdleTimer() {
        idleTimer_.expires_after(config().idleTimeout);
        idleTimer_.async_wait([self = shared_from_this()](asio::error_code ec) {
            if (ec == asio::error::operation_aborted || self->closed_) return;
            if (self->busy_ || self->writing_) return self->armIdleTimer();  // a long request is not idleness
            self->close("idle timeout");
        });
    }

    void close(const std::string& reason) {
        if (closed_) return;
        closed_ = true;
        idleTimer_.cancel();
        asio::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        server_->sessionClosed(info_.id, served_, reason);
    }

    std::shared_ptr<TcpServer::Impl> server_;
    tcp::socket socket_;
    asio::steady_timer idleTimer_;
    ConnectionInfo info_;

    std::array<char, 16 * 1024> readBuf_{};
    std::string pending_;                 // bytes after the last \0
    std::deque<std::string> requests_;    // complete, not yet handled
    std::deque<std::string> writeQueue_;  // replies being sent
    std::optional<std::string> errorReply_;
    std::string protocolReason_;
    std::uint64_t served_ = 0;

    bool reading_ = false;
    bool writing_ = false;
    bool busy_ = false;  // a request is with a worker
    bool eof_ = false;
    bool closeRequested_ = false;
    bool protocolFailed_ = false;
    bool closed_ = false;
};

// ---- Accepting ---------------------------------------------------------------

void TcpServer::Impl::accept() {
    acceptor.async_accept(asio::make_strand(io), [self = shared_from_this()](asio::error_code ec, tcp::socket socket) {
        if (self->stopping) return;
        if (ec) {
            if (ec == asio::error::operation_aborted) return;
            // e.g. too many open files: back off instead of spinning.
            if (auto suppressed = self->acceptLog.allow())
                self->log->warn("Accept failed: {}; retrying{}", ec.message(), log::suppressedSuffix(*suppressed));
            self->acceptRetry.expires_after(std::chrono::milliseconds(100));
            self->acceptRetry.async_wait([self](asio::error_code e) {
                if (!e && !self->stopping) self->accept();
            });
            return;
        }

        asio::error_code ignored;
        if (self->activeCount() >= self->config.maxConnections) {
            ++self->rejectedConnections;
            if (auto suppressed = self->limitLog.allow())
                self->log->warn("Connection limit ({}) reached; rejecting new connections{}",
                                self->config.maxConnections, log::suppressedSuffix(*suppressed));
            socket.close(ignored);
            return self->accept();
        }

        socket.set_option(tcp::no_delay(true), ignored);  // request/response: latency over batching
        const std::uint64_t id = self->nextId++;
        auto session = std::make_shared<Session>(self, std::move(socket), id);
        {
            std::lock_guard<std::mutex> lock(self->sessionsMutex);
            self->sessions.emplace(id, session);
        }
        ++self->totalConnections;
        self->log->debug("Connection #{} opened from {}:{}", id, session->info().remoteAddress,
                         session->info().remotePort);
        session->start();
        self->accept();
    });
}

void TcpServer::Impl::sessionClosed(std::uint64_t id, std::uint64_t served, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(sessionsMutex);
        sessions.erase(id);
    }
    sessionsCv.notify_all();
    log->debug("Connection #{} closed after {} requests: {}", id, served, reason);
}

// ---- TcpServer ---------------------------------------------------------------

TcpServer::TcpServer(TcpServerConfig config, std::shared_ptr<IMessageHandler> handler) {
    if (!handler) throw NetError("TcpServer: handler is null");
    if (config.ioThreads == 0 || config.workerThreads == 0) throw NetError("TcpServer: thread counts must be > 0");
    if (config.maxPendingRequests == 0) throw NetError("TcpServer: maxPendingRequests must be > 0");
    impl_ = std::make_shared<Impl>(std::move(config), std::move(handler));
}

TcpServer::~TcpServer() { stop(); }

void TcpServer::start() {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.lifecycleMutex);
    if (s.started) throw NetError("TcpServer: already started");

    const std::string where = s.config.bindAddress + ":" + std::to_string(s.config.port);
    try {
        const tcp::endpoint endpoint(asio::ip::make_address(s.config.bindAddress), s.config.port);
        s.acceptor.open(endpoint.protocol());
        s.acceptor.set_option(tcp::acceptor::reuse_address(true));
        s.acceptor.bind(endpoint);
        s.acceptor.listen(asio::socket_base::max_listen_connections);
        s.boundPort = s.acceptor.local_endpoint().port();
    } catch (const std::system_error& e) {
        asio::error_code ignored;
        s.acceptor.close(ignored);
        throw NetError("Cannot listen on " + where + ": " + e.what());
    }

    s.workers = std::make_unique<asio::thread_pool>(s.config.workerThreads);
    s.work.emplace(s.io.get_executor());
    for (std::size_t i = 0; i < s.config.ioThreads; ++i) {
        s.ioThreads.emplace_back([impl = impl_] {
            try {
                impl->io.run();
            } catch (const std::exception& e) {
                impl->log->critical("I/O thread terminated by exception: {}", e.what());
            }
        });
    }
    s.started = true;
    s.accept();
    s.log->info("Listening on {}:{} ({} I/O threads, {} workers, max {} connections)", s.config.bindAddress,
                s.boundPort, s.config.ioThreads, s.config.workerThreads, s.config.maxConnections);
}

void TcpServer::stop() {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.lifecycleMutex);
    if (!s.started || s.stopped) return;
    s.stopped = true;
    s.stopping = true;
    s.log->info("Stopping server ({} open connections)", s.activeCount());

    asio::post(s.io, [impl = impl_] {
        asio::error_code ignored;
        impl->acceptor.close(ignored);
        impl->acceptRetry.cancel();
    });

    for (auto& session : s.liveSessions()) session->requestClose();
    {
        std::unique_lock<std::mutex> sessionsLock(s.sessionsMutex);
        if (!s.sessionsCv.wait_for(sessionsLock, s.config.shutdownTimeout, [&] { return s.sessions.empty(); })) {
            s.log->warn("{} connections still busy after {}s; closing them", s.sessions.size(),
                        s.config.shutdownTimeout.count());
            sessionsLock.unlock();
            for (auto& session : s.liveSessions()) session->forceClose();
            sessionsLock.lock();
            s.sessionsCv.wait_for(sessionsLock, std::chrono::seconds(2), [&] { return s.sessions.empty(); });
        }
    }

    s.workers->join();  // a forcibly closed connection may still have a request running
    s.work.reset();     // io.run() returns once the remaining (aborted) operations are done
    for (auto& t : s.ioThreads) t.join();
    s.ioThreads.clear();

    s.log->info("Server stopped: {} requests on {} connections ({} rejected, {} handler errors, {} protocol errors)",
                s.requests.load(), s.totalConnections.load(), s.rejectedConnections.load(), s.handlerErrors.load(),
                s.protocolErrors.load());
}

std::uint16_t TcpServer::port() const { return impl_->boundPort; }

TcpServer::Stats TcpServer::stats() const {
    Impl& s = *impl_;
    return {s.activeCount(),   s.totalConnections.load(), s.rejectedConnections.load(),
            s.requests.load(), s.handlerErrors.load(),    s.protocolErrors.load()};
}

}  // namespace caelitus::net
