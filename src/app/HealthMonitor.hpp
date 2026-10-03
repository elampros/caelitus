#pragma once

/// @file
/// The "health" job: checks every part of the server and logs what is wrong.
/// @ingroup app

#include "caelitus/api/OperationsApi.hpp"
#include "caelitus/catalog/service/BookCache.hpp"
#include "caelitus/catalog/service/ReactionService.hpp"
#include "caelitus/config/AppConfig.hpp"
#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/SqlExecutor.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"
#include "caelitus/mqtt/IMqttClient.hpp"
#include "caelitus/net/TcpServer.hpp"
#include "caelitus/scheduler/Scheduler.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace caelitus::app {

/// Checks the server's health and keeps the latest report for `system.health`.
///
/// check() runs as the "health" job (default every 15 s). It measures:
///
/// | Part        | Measured                                     | A problem when |
/// |-------------|----------------------------------------------|----------------|
/// | Database    | a `SELECT 1` round trip, pool connections    | the query fails; it is slower than health.slowDatabaseMs; every connection is in use |
/// | MQTT        | connection, messages sent/dropped/received   | not connected |
/// | Book cache  | entries, estimated memory, hits/misses       | (informational) |
/// | TCP server  | connections, requests, errors                | (informational) |
/// | Likes       | buffered book-days, drops                    | the buffer is fuller than health.reactionBufferWarnPercent; likes were dropped since the last check |
/// | Process     | resident memory, threads, uptime             | memory above health.maxMemoryMb (when set) |
/// | Jobs        | paused, running, failing                     | another job's last run failed |
///
/// Logging ("health" logger), so the log tells a story without repeating it
/// every 15 seconds:
/// - a new problem: warn at once, then at most every 5 minutes while it lasts;
/// - a problem that went away: info ("Resolved: ...");
/// - every check: one debug line with the main figures.
class HealthMonitor {
public:
    /// The parts it watches; all must outlive the monitor.
    struct Sources {
        std::shared_ptr<db::ConnectionPool> pool;             ///< For connection counts.
        std::size_t poolMax = 0;                              ///< The pool's maxSize.
        std::shared_ptr<db::SqlExecutor> sql;                 ///< For the test query.
        std::shared_ptr<catalog::BookCache> bookCache;        ///< Cache figures.
        std::shared_ptr<catalog::ReactionService> reactions;  ///< Like buffer figures.
        std::shared_ptr<mqtt::IMqttClient> mqtt;              ///< Broker connection and counters.
        const net::TcpServer* server = nullptr;               ///< May be null before the server starts.
        const scheduler::Scheduler* scheduler = nullptr;      ///< May be null.
    };

    /// @param sources    What to check.
    /// @param thresholds The "health" configuration section.
    /// @param startedAt  When the server started, for the uptime.
    HealthMonitor(Sources sources, AppConfig::Health thresholds, Timestamp startedAt);

    /// Runs every check, updates the latest report and logs changes. Never
    /// throws: a failing check is itself reported as a problem.
    void check();

    /// The latest report; std::nullopt before the first check().
    std::optional<api::HealthReport> latest() const;

    /// Lets the monitor see the TCP server once it exists.
    void setServer(const net::TcpServer* server);

private:
    void logChanges(const std::map<std::string, std::string>& problems);

    Sources s_;
    AppConfig::Health thresholds_;
    Timestamp startedAt_;
    log::Logger log_;

    mutable std::mutex mutex_;
    std::optional<api::HealthReport> latest_;
    std::map<std::string, std::string> active_;  // problem key -> message, from the previous check
    std::map<std::string, std::unique_ptr<log::LogThrottle>> reminders_;
    std::int64_t lastDropped_ = 0;
};

}  // namespace caelitus::app
