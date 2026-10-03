#pragma once

/// @file
/// Rate limiting for repeating log messages.
/// @ingroup log

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace caelitus::log {

/// Lets a repeating log message through at most once per interval and counts
/// what it held back, so a failure loop produces one line a minute instead of
/// thousands:
///
/// @code
/// if (auto suppressed = dropThrottle_.allow())
///     log_->warn("Message dropped: not connected{}", log::suppressedSuffix(*suppressed));
/// @endcode
///
/// Thread-safe.
class LogThrottle {
public:
    /// @param interval  Minimum time between two allowed messages.
    explicit LogThrottle(std::chrono::steady_clock::duration interval = std::chrono::seconds(60))
        : interval_(interval) {}

    /// Decides whether this occurrence should be logged.
    /// @return The number of occurrences suppressed since the last allowed one
    ///         if this one should be logged; std::nullopt if it should not.
    std::optional<std::uint64_t> allow() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (last_ && now - *last_ < interval_) {
            ++suppressed_;
            return std::nullopt;
        }
        last_ = now;
        const std::uint64_t suppressed = suppressed_;
        suppressed_ = 0;
        return suppressed;
    }

    /// Forgets the history, e.g. once the condition being logged has cleared,
    /// so the next occurrence is logged immediately.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_.reset();
        suppressed_ = 0;
    }

private:
    std::mutex mutex_;
    std::chrono::steady_clock::duration interval_;
    std::optional<std::chrono::steady_clock::time_point> last_;
    std::uint64_t suppressed_ = 0;
};

/// Text to append to a throttled message: empty for 0, otherwise
/// " (+N similar since last report)".
inline std::string suppressedSuffix(std::uint64_t suppressed) {
    return suppressed == 0 ? std::string() : " (+" + std::to_string(suppressed) + " similar since last report)";
}

}  // namespace caelitus::log
