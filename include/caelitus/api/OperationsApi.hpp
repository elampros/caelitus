#pragma once

/// @file
/// JSON-RPC methods for running the server: scheduled jobs and health.
/// @ingroup api

#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/core/DateTime.hpp"
#include "caelitus/scheduler/Scheduler.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace caelitus::api {

/// The server's health, as returned by `system.health`.
///
/// Filled by the "health" job (app::HealthMonitor) every 15 seconds; the API
/// returns the latest report, so calling it is cheap.
struct HealthReport {
    std::string status = "ok";          ///< "ok", or "degraded" when there are problems.
    std::vector<std::string> problems;  ///< What is wrong, one sentence each; empty when ok.
    Timestamp checkedAt{};              ///< When the report was made.
    std::int64_t uptimeSeconds = 0;     ///< Since the server started.
    std::string version;                ///< Build version.

    /// The database, seen through the connection pool.
    struct Database {
        bool up = false;                  ///< A test query succeeded.
        std::optional<double> pingMs;     ///< Its round trip; none when down.
        std::size_t openConnections = 0;  ///< Open pool connections (idle + in use).
        std::size_t idleConnections = 0;  ///< Of which idle.
        std::size_t maxConnections = 0;   ///< The pool limit.
    } database;                           ///< Database figures.

    /// The MQTT client.
    struct Mqtt {
        bool connected = false;            ///< Connected to the broker.
        std::uint64_t published = 0;       ///< Messages sent since start.
        std::uint64_t publishDropped = 0;  ///< Not sent (no connection, send failed).
        std::uint64_t received = 0;        ///< Messages received since start.
        std::uint64_t receiveDropped = 0;  ///< Dropped because the incoming queue was full.
    } mqtt;                                ///< MQTT figures.

    /// The in-memory book cache.
    struct BookCache {
        std::size_t books = 0;        ///< Entries.
        std::size_t approxBytes = 0;  ///< Estimated memory use.
        std::uint64_t hits = 0;       ///< Lookups that found their book, since start.
        std::uint64_t misses = 0;     ///< Lookups that did not.
    } bookCache;                      ///< Book cache figures.

    /// The TCP server.
    struct Server {
        std::uint64_t activeConnections = 0;  ///< Open now.
        std::uint64_t totalConnections = 0;   ///< Accepted since start.
        std::uint64_t requests = 0;           ///< Requests handled since start.
        std::uint64_t handlerErrors = 0;      ///< Requests that failed inside the server.
        std::uint64_t protocolErrors = 0;     ///< Unreadable input.
    } server;                                 ///< TCP server figures.

    /// The like/dislike buffer.
    struct Reactions {
        std::size_t pending = 0;   ///< Book-days waiting for the next flush.
        std::size_t capacity = 0;  ///< The buffer limit.
        std::int64_t dropped = 0;  ///< Likes dropped because the buffer was full, since start.
    } reactions;                   ///< Like buffer figures.

    /// The server process.
    struct Process {
        std::optional<std::uint64_t> memoryBytes;  ///< Resident memory (RSS); none if unknown.
        std::optional<int> threads;                ///< Threads; none if unknown.
    } process;                                     ///< Process figures.

    /// The scheduled jobs.
    struct Jobs {
        std::size_t total = 0;    ///< Jobs defined.
        std::size_t paused = 0;   ///< Of which paused.
        std::size_t running = 0;  ///< Running right now.
        std::size_t failing = 0;  ///< Whose last run failed.
    } jobs;                       ///< Scheduled job figures.
};

/// Where `system.health` gets the latest report; std::nullopt before the first check.
using HealthSource = std::function<std::optional<HealthReport>()>;

/// Registers the operations methods:
///
/// | Method             | Does |
/// |--------------------|------|
/// | `scheduler.list`   | Every job with its schedule, state and history |
/// | `scheduler.get`    | One job |
/// | `scheduler.run`    | Runs a job now (also while paused); Conflict "job_running" if it is running |
/// | `scheduler.pause`  | Stops a job's scheduled runs (until resumed or the server restarts) |
/// | `scheduler.resume` | Resumes a paused job |
/// | `system.health`    | The latest health report |
///
/// Pausing is not persisted: after a restart a job is paused only if the
/// configuration says so (`"enabled": false`). The dependencies are only used
/// when a method runs, so null ones are fine for generating the OpenRPC document.
void registerOperationsApi(JsonRpcHandler& rpc, std::shared_ptr<scheduler::Scheduler> scheduler, HealthSource health);

}  // namespace caelitus::api
