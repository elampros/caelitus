#pragma once

/// @file
/// Logger configuration and access.
/// @ingroup log

#include <spdlog/spdlog.h>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace caelitus::log {

/// A named spdlog logger. Use it like `log_->info("Connected to {}", host)`
/// (fmt syntax).
using Logger = std::shared_ptr<spdlog::logger>;

/// Where log output goes and how much of it; the "log" configuration section.
struct LogConfig {
    /// Default level: trace, debug, info, warn, error, critical or off.
    std::string level = "info";
    /// Per-logger overrides, e.g. `{{"db.sql", "trace"}, {"db.pool", "debug"}}`.
    std::map<std::string, std::string> levels;

    bool console = true;                              ///< Log to stdout (colored).
    std::string filePath;                             ///< Log file; empty: no file output.
    std::size_t maxFileSizeBytes = 10 * 1024 * 1024;  ///< Rotate the file at this size.
    std::size_t maxFiles = 5;                         ///< Rotated files to keep.

    /// spdlog pattern: time, level, logger name, thread id, message.
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] [t%t] %v";

    /// Additional destinations (syslog, tests, ...).
    std::vector<spdlog::sink_ptr> extraSinks;
};

/// Configures output for all loggers, including ones already handed out.
/// Call once at startup, before other threads start logging.
/// @throws std::invalid_argument for an unknown level name.
void init(const LogConfig& config);

/// Returns the named logger, sharing the configured sinks.
///
/// Names are dotted by module ("db.sql", "mqtt", "tcp"), so their levels can be
/// set per area. Cheap to call, but components should fetch their logger once
/// (e.g. in the constructor) and keep it.
Logger get(const std::string& name);

/// True for trace, debug, info, warn, error, critical and off.
bool isValidLevel(const std::string& level) noexcept;

/// Flushes and releases all loggers. Call before exit.
void shutdown();

}  // namespace caelitus::log
