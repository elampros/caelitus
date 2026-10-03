#pragma once

/// @file
/// When a job runs: every N, at a fixed rate, or on a cron expression.
/// @ingroup scheduler

#include "caelitus/core/DateTime.hpp"
#include "caelitus/core/TimeZone.hpp"

#include <bitset>
#include <chrono>
#include <string>

namespace caelitus::scheduler {

/// A cron expression: `minute hour day-of-month month day-of-week`.
///
/// Each field is `*`, a number, a range `a-b`, a step `*/n` or `a-b/n`, or a
/// comma-separated list of those. Months and weekdays also accept three-letter
/// English names (`jan`, `mon`); weekday 0 and 7 are both Sunday. As in classic
/// cron, when both day-of-month and day-of-week are restricted, a day matching
/// **either** one qualifies.
///
/// @code
/// "0 3 * * *"        every day at 03:00
/// "*/15 * * * *"     every quarter of an hour
/// "30 8 * * mon-fri" weekdays at 08:30
/// "0 0 1 * *"        the first of every month at midnight
/// @endcode
///
/// Times are local to the TimeZone the expression is evaluated in. On the day
/// clocks go forward, a time in the skipped hour does not happen that day; on
/// the day they go back, a time in the repeated hour happens once (the first
/// time).
class CronExpression {
public:
    /// An empty expression that matches nothing; use parse().
    CronExpression() = default;

    /// @throws std::invalid_argument naming the field and the problem.
    static CronExpression parse(const std::string& text);

    /// The first matching minute strictly after `after`, as UTC.
    /// @throws std::runtime_error if nothing matches within 5 years (e.g. "0 0 31 2 *").
    Timestamp nextAfter(Timestamp after, const TimeZone& zone) const;

    /// The expression as written.
    const std::string& text() const noexcept { return text_; }

private:
    bool matchesDay(const Date& day) const;

    std::string text_;
    std::bitset<60> minutes_;
    std::bitset<24> hours_;
    std::bitset<32> daysOfMonth_;  // 1..31
    std::bitset<13> months_;       // 1..12
    std::bitset<7> daysOfWeek_;    // 0 = Sunday
    bool anyDayOfMonth_ = true;
    bool anyDayOfWeek_ = true;
};

/// When a job runs.
///
/// | Text form           | Meaning |
/// |---------------------|---------|
/// | `every 15s`         | 15 seconds after the previous run **finished** (runs never pile up) |
/// | `rate 1s`           | every second, measured from the previous **planned** start; missed runs are skipped, not caught up |
/// | `cron 0 3 * * *`    | at matching local times (see CronExpression) |
///
/// Durations take the units `ms`, `s`, `min`, `h` and `d` (`every 5min`,
/// `rate 500ms`, `every 1h`).
class Schedule {
public:
    /// The three kinds of schedule.
    enum class Kind {
        Every,  ///< A fixed delay after the previous run ends.
        Rate,   ///< A fixed rate, aligned to the first planned start.
        Cron    ///< Calendar times from a CronExpression.
    };

    /// A fixed delay between runs. @throws std::invalid_argument if not positive.
    static Schedule every(std::chrono::milliseconds interval);
    /// A fixed rate. @throws std::invalid_argument if not positive.
    static Schedule rate(std::chrono::milliseconds interval);
    /// Calendar times in `zone`. @throws std::invalid_argument for a bad expression.
    static Schedule cron(const std::string& expression, TimeZone zone);

    /// Parses the text form ("every 15s", "rate 1s", "cron 0 3 * * *").
    /// @param text  The schedule, as written in the configuration.
    /// @param zone  Time zone for cron schedules.
    /// @throws std::invalid_argument with a readable reason.
    static Schedule parse(const std::string& text, const TimeZone& zone);

    /// Parses a duration such as "15s", "500ms", "5min", "2h", "1d".
    /// @throws std::invalid_argument
    static std::chrono::milliseconds parseDuration(const std::string& text);

    Kind kind() const noexcept { return kind_; }  ///< Which kind of schedule.
    /// The interval of Every and Rate schedules; zero for Cron.
    std::chrono::milliseconds interval() const noexcept { return interval_; }

    /// The next start time.
    /// @param reference  Every: when the previous run ended. Rate: the previous
    ///                   planned start. Cron: "now" (the next match after it).
    /// For the first run of Every and Rate, pass the time the scheduler started.
    Timestamp nextAfter(Timestamp reference) const;

    /// The canonical text form, e.g. "every 15s" or "cron 0 3 * * * (Europe/Athens)".
    std::string text() const;

private:
    Schedule(Kind kind, std::chrono::milliseconds interval, CronExpression cron, TimeZone zone);

    Kind kind_;
    std::chrono::milliseconds interval_;
    CronExpression cron_;
    TimeZone zone_;
};

/// A duration in the shortest exact unit: "500ms", "15s", "5min", "2h", "1d".
std::string formatDuration(std::chrono::milliseconds d);

}  // namespace caelitus::scheduler
