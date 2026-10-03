#pragma once

/// @file
/// The application configuration (config/config.json).
/// @ingroup config

#include "caelitus/db/DbConfig.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/mqtt/MqttConfig.hpp"
#include "caelitus/net/TcpServerConfig.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace caelitus {

/// Invalid or missing configuration.
///
/// The message names the file and the offending key, e.g.
/// "config/config.json: db.pool.maxSize: must be > 0".
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The whole application configuration, read from config/config.json.
///
/// Rules:
/// - Missing keys take the defaults below; keys marked "required" must exist.
/// - Unknown keys are errors, so a typo ("prot") never goes unnoticed.
/// - Any string value of the form "${NAME}" is replaced by environment
///   variable NAME (error if unset); use it for passwords.
/// - Durations are integers whose key says the unit ("connectTimeoutMs").
/// - `//` and `/* */` comments are allowed.
///
/// Every value is validated on load (ranges, enum values, time zone names), so
/// a bad configuration stops the server at startup with a clear message.
struct AppConfig {
    /// "mqtt" section: server (required), port, clientId, username, password,
    /// keepAliveSec, qos, retain, cleanSession, reconnectMinDelaySec,
    /// reconnectMaxDelaySec, incomingQueueSize,
    /// will { topic (required), payload, onlinePayload, qos, retain }.
    using Mqtt = mqtt::MqttConfig;

    /// "db" section.
    struct Database {
        /// host, port, user (required), password, database (required),
        /// connectTimeoutMs, socketTimeoutMs, sessionTimeZone, initStatements.
        db::DbConfig connection;
        db::PoolConfig pool;          ///< "db.pool": maxSize, acquireTimeoutMs, validateAfterIdleMs.
        db::ExecutorConfig executor;  ///< "db.slowQueryMs".
        db::TransactionConfig
            transaction;  ///< "db.transaction": maxAttempts, initialBackoffMs, backoffMultiplier, slowTransactionMs.
    };

    /// "server" section: port (required), bindAddress, ioThreads,
    /// workerThreads, maxConnections, maxMessageBytes, maxPendingRequests,
    /// idleTimeoutSec, slowRequestMs, shutdownTimeoutSec, log.
    struct Server : net::TcpServerConfig {
        std::string log = "info";  ///< Level of the "server" logger.
    };

    /// "catalog" section (optional): timeZone,
    /// reactions { topicPrefix, maxBuffered, keepDays, topTopic }.
    struct Catalog {
        /// Decides what "today" means for likes/dislikes. UTC or a European zone.
        std::string timeZone = "Europe/Athens";
        /// Likes arrive on "<prefix>/<bookId>/like" and ".../dislike".
        std::string reactionTopicPrefix = "catalog/in/books";
        /// (book, day) entries kept in memory between flushes.
        std::size_t maxBufferedReactions = 100000;
        /// Per-day like counts older than this are deleted by the
        /// "reaction-cleanup" job; at least 366, so "last year" stays exact.
        /// The all-time totals on each book are kept.
        int reactionKeepDays = 400;
        /// Where the "top-books" job publishes today's top 10 (retained JSON).
        std::string topBooksTopic = "catalog/stats/top-today";
    };

    /// Overrides for one scheduled job ("scheduler.jobs.NAME"); unset
    /// fields keep the job's built-in defaults.
    struct JobSettings {
        std::optional<std::string> schedule;  ///< "every 15s", "rate 1s", "cron 0 3 * * *" (catalog.timeZone).
        std::optional<bool> enabled;          ///< false: starts paused.
        std::optional<std::chrono::milliseconds> timeout;     ///< "timeoutSec"; 0: none.
        std::optional<std::chrono::milliseconds> jitter;      ///< "jitterSec".
        std::optional<int> retryAttempts;                     ///< "retryAttempts".
        std::optional<std::chrono::milliseconds> retryDelay;  ///< "retryDelaySec".
    };

    /// "scheduler" section (optional): threads, jobs { NAME: JobSettings }.
    /// Job names are checked against the jobs the application defines at
    /// startup (see app::Application).
    struct Scheduler {
        std::size_t threads = 2;                  ///< Jobs that can run at the same time.
        std::map<std::string, JobSettings> jobs;  ///< Per-job overrides, by job name.
    };

    /// "health" section (optional): thresholds of the "health" job.
    struct Health {
        /// A database round trip slower than this is a problem.
        std::chrono::milliseconds slowDatabase{500};
        /// Resident memory above this is a problem; 0: not checked.
        std::size_t maxMemoryMb = 0;
        /// A like buffer fuller than this (percent of maxBuffered) is a problem.
        int reactionBufferWarnPercent = 80;
    };

    Mqtt mqtt;            ///< "mqtt" section.
    Database db;          ///< "db" section.
    Catalog catalog;      ///< "catalog" section.
    Scheduler scheduler;  ///< "scheduler" section.
    Health health;        ///< "health" section.
    Server server;        ///< "server" section.
    log::LogConfig log;   ///< "log" section (optional): level, levels, console, file, maxFileSizeMb, maxFiles, pattern.

    /// Parses and validates JSON text.
    /// @param text    The configuration, JSON with comments.
    /// @param source  Used in error messages (usually the file name).
    /// @throws ConfigError
    static AppConfig fromJson(std::string_view text, const std::string& source = "config");

    /// Reads, parses and validates a file.
    /// @throws ConfigError, also if the file cannot be read.
    static AppConfig load(const std::filesystem::path& file);

    /// Finds config.json. Looked for, in order:
    /// 1. `--config <path>` (or `--config=<path>`) on the command line;
    /// 2. `$CAELITUS_CONFIG`;
    /// 3. config/config.json under the working directory;
    /// 4. config/config.json in the executable's directory or any directory
    ///    above it (covers running from `build/<preset>/src` or CLion's
    ///    cmake-build-*).
    ///
    /// An explicit choice (1, 2) must exist; there is no silent fallback.
    /// @throws ConfigError listing every place tried.
    static std::filesystem::path locate(int argc, const char* const* argv);
};

}  // namespace caelitus
