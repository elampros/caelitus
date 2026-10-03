#pragma once

// Log capture for tests: route loggers to a CaptureSink and assert on lines.

#include <spdlog/sinks/base_sink.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace test {

// Captures log output so tests can assert on it.
class CaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    bool contains(spdlog::level::level_enum level, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [l, line] : lines_)
            if (l == level && line.find(text) != std::string::npos) return true;
        return false;
    }
    std::size_t count(spdlog::level::level_enum level, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& [l, line] : lines_)
            if (l == level && line.find(text) != std::string::npos) ++n;
        return n;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        lines_.emplace_back(msg.level, std::string(msg.logger_name.begin(), msg.logger_name.end()) + ": " +
                                           std::string(msg.payload.begin(), msg.payload.end()));
    }
    void flush_() override {}

private:
    std::vector<std::pair<spdlog::level::level_enum, std::string>> lines_;
};

}  // namespace test
