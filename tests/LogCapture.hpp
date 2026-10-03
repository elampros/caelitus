#pragma once

/// @file
/// Log capture for tests: route loggers to a CaptureSink and assert on lines.
///
/// @code
/// auto logs = std::make_shared<test::CaptureSink>();
/// caelitus::log::LogConfig config;
/// config.console = false;
/// config.extraSinks = {logs};
/// caelitus::log::init(config);
/// ...
/// CHECK(logs->contains(spdlog::level::warn, "Slow request"));
/// @endcode
/// @ingroup tests

#include <spdlog/sinks/base_sink.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace test {

/// A thread-safe spdlog sink that keeps every line in memory, so tests can
/// assert on what was logged. Lines are stored as `logger: message`.
class CaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// True if some line at exactly `level` contains `text`.
    bool contains(spdlog::level::level_enum level, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [l, line] : lines_)
            if (l == level && line.find(text) != std::string::npos) return true;
        return false;
    }
    /// How many lines at exactly `level` contain `text` (for throttling checks).
    std::size_t count(spdlog::level::level_enum level, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& [l, line] : lines_)
            if (l == level && line.find(text) != std::string::npos) ++n;
        return n;
    }
    /// Forgets every line captured so far.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }

protected:
    /// Stores one formatted line (called by spdlog with the sink's mutex held).
    void sink_it_(const spdlog::details::log_msg& msg) override {
        lines_.emplace_back(msg.level, std::string(msg.logger_name.begin(), msg.logger_name.end()) + ": " +
                                           std::string(msg.payload.begin(), msg.payload.end()));
    }
    /// Nothing to flush: lines are kept in memory.
    void flush_() override {}

private:
    std::vector<std::pair<spdlog::level::level_enum, std::string>> lines_;
};

}  // namespace test
