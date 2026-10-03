#include "app/HealthMonitor.hpp"

#include "caelitus/Version.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace caelitus::app {

namespace {

// Resident memory from /proc/self/statm (second field, in pages); Linux only.
std::optional<std::uint64_t> residentBytes() {
    std::ifstream in("/proc/self/statm");
    std::uint64_t size = 0, resident = 0;
    if (!(in >> size >> resident)) return std::nullopt;
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? std::optional<std::uint64_t>(resident * static_cast<std::uint64_t>(page)) : std::nullopt;
}

// Threads of this process: the entries of /proc/self/task.
std::optional<int> threadCount() {
    std::error_code ec;
    int n = 0;
    for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end; it.increment(ec)) ++n;
    return ec || n == 0 ? std::nullopt : std::optional<int>(n);
}

std::string megabytes(std::uint64_t bytes) { return fmt::format("{:.0f} MB", static_cast<double>(bytes) / 1048576.0); }

}  // namespace

HealthMonitor::HealthMonitor(Sources sources, AppConfig::Health thresholds, Timestamp startedAt)
    : s_(std::move(sources)),
      thresholds_(thresholds),
      startedAt_(startedAt),
      log_(log::get("health")) {}

void HealthMonitor::setServer(const net::TcpServer* server) {
    std::lock_guard<std::mutex> lock(mutex_);
    s_.server = server;
}

void HealthMonitor::check() {
    api::HealthReport r;
    std::map<std::string, std::string> problems;  // key -> message
    const Timestamp now = nowUtc();
    r.checkedAt = now;
    r.uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - startedAt_).count();
    r.version = kVersion;

    // Database: a real round trip through the pool.
    try {
        const auto start = std::chrono::steady_clock::now();
        s_.sql->queryScalar<int>("SELECT 1");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        r.database.up = true;
        r.database.pingMs = ms;
        if (ms >= static_cast<double>(thresholds_.slowDatabase.count()))
            problems["db.slow"] = fmt::format("database is slow: a test query took {:.0f} ms (threshold {} ms)", ms,
                                              thresholds_.slowDatabase.count());
    } catch (const std::exception& e) {
        problems["db.down"] = std::string("database unreachable: ") + e.what();
    }
    const auto pool = s_.pool->stats();
    r.database.openConnections = pool.total;
    r.database.idleConnections = pool.idle;
    r.database.maxConnections = s_.poolMax;
    if (s_.poolMax > 0 && pool.total >= s_.poolMax && pool.idle == 0)
        problems["db.pool"] = fmt::format("all {} database connections are in use", s_.poolMax);

    // MQTT.
    const auto m = s_.mqtt->stats();
    r.mqtt = {s_.mqtt->isConnected(), m.published, m.publishDropped, m.received, m.receiveDropped};
    if (!r.mqtt.connected) problems["mqtt"] = "MQTT broker not connected: likes are not received, events not sent";

    // Book cache.
    const auto c = s_.bookCache->stats();
    r.bookCache = {c.size, s_.bookCache->approxBytes(), c.hits, c.misses};

    // TCP server.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (s_.server) {
            const auto t = s_.server->stats();
            r.server = {t.activeConnections, t.totalConnections, t.requests, t.handlerErrors, t.protocolErrors};
        }
    }

    // Like buffer.
    const auto b = s_.reactions->bufferStats();
    r.reactions = {b.pending, b.capacity, b.dropped};
    if (b.capacity > 0) {
        const double percent = 100.0 * static_cast<double>(b.pending) / static_cast<double>(b.capacity);
        if (percent >= thresholds_.reactionBufferWarnPercent)
            problems["likes.buffer"] = fmt::format(
                "like buffer {:.0f}% full ({} of {} book-days): is the database "
                "keeping up?",
                percent, b.pending, b.capacity);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (b.dropped > lastDropped_)
            problems["likes.dropped"] =
                fmt::format("{} likes dropped since the last check (buffer full)", b.dropped - lastDropped_);
        lastDropped_ = b.dropped;
    }

    // Process.
    r.process.memoryBytes = residentBytes();
    r.process.threads = threadCount();
    if (thresholds_.maxMemoryMb > 0 && r.process.memoryBytes &&
        *r.process.memoryBytes > thresholds_.maxMemoryMb * 1024 * 1024)
        problems["memory"] = fmt::format("memory use {} is above the limit of {} MB", megabytes(*r.process.memoryBytes),
                                         thresholds_.maxMemoryMb);

    // Jobs (except this one).
    if (s_.scheduler) {
        for (const auto& j : s_.scheduler->list()) {
            ++r.jobs.total;
            if (j.paused) ++r.jobs.paused;
            if (j.running) ++r.jobs.running;
            if (j.lastResult == "failed") {
                ++r.jobs.failing;
                if (j.name != "health")
                    problems["job." + j.name] =
                        fmt::format("job '{}' failed ({} in a row): {}", j.name, j.consecutiveFailures, j.lastError);
            }
        }
    }

    for (const auto& [key, message] : problems) r.problems.push_back(message);
    r.status = problems.empty() ? "ok" : "degraded";
    log_->debug(
        "{}: db {} ({} of {} connections), mqtt {}, cache {} books ~{} KB, likes pending {}, "
        "memory {}, {} threads, uptime {} s",
        r.status, r.database.up ? fmt::format("{:.1f} ms", *r.database.pingMs) : "down", r.database.openConnections,
        r.database.maxConnections, r.mqtt.connected ? "up" : "down", r.bookCache.books, r.bookCache.approxBytes / 1024,
        r.reactions.pending, r.process.memoryBytes ? megabytes(*r.process.memoryBytes) : "?",
        r.process.threads.value_or(0), r.uptimeSeconds);

    logChanges(problems);
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = std::move(r);
}

void HealthMonitor::logChanges(const std::map<std::string, std::string>& problems) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, message] : problems) {
        auto& reminder = reminders_[key];
        if (!reminder) reminder = std::make_unique<log::LogThrottle>(std::chrono::minutes(5));
        if (!active_.count(key)) reminder->reset();  // new: log at once
        if (auto suppressed = reminder->allow()) log_->warn("Health: {}", message);
    }
    for (const auto& [key, message] : active_)
        if (!problems.count(key)) log_->info("Health: resolved: {}", message);
    active_ = problems;
}

std::optional<api::HealthReport> HealthMonitor::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

}  // namespace caelitus::app
