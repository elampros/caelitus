/// @file
/// Parsing of `every`, `rate` and `cron` schedules, and computing the next run
/// (cron in local time, across summer-time changes).
/// @ingroup scheduler

#include "caelitus/scheduler/Schedule.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace caelitus::scheduler {

namespace {

using std::chrono::milliseconds;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(text);
    while (std::getline(in, item, sep)) out.push_back(item);
    return out;
}

// One cron field ("*/15", "1-5", "mon,wed") into the set of values it allows.
template <std::size_t N>
std::bitset<N> parseField(const std::string& field, const char* name, int min, int max,
                          const std::vector<std::string>& names = {}) {
    auto fail = [&](const std::string& why) -> std::invalid_argument {
        return std::invalid_argument(std::string("cron ") + name + " field '" + field + "': " + why);
    };
    auto value = [&](const std::string& text) {
        const std::string t = lower(text);
        for (std::size_t i = 0; i < names.size(); ++i)
            if (t == names[i]) return min + static_cast<int>(i);
        if (t.empty() || !std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); }))
            throw fail("'" + text + "' is not a number");
        const int v = std::stoi(t);
        if (v < min || v > max)
            throw fail(std::to_string(v) + " is outside " + std::to_string(min) + "-" + std::to_string(max));
        return v;
    };

    std::bitset<N> bits;
    if (field.empty()) throw fail("empty");
    for (const std::string& part : split(field, ',')) {
        std::string range = part;
        int step = 1;
        if (const auto slash = part.find('/'); slash != std::string::npos) {
            range = part.substr(0, slash);
            const std::string stepText = part.substr(slash + 1);
            if (stepText.empty() ||
                !std::all_of(stepText.begin(), stepText.end(), [](unsigned char c) { return std::isdigit(c); }))
                throw fail("step '" + stepText + "' is not a number");
            step = std::stoi(stepText);
            if (step < 1) throw fail("step must be at least 1");
        }
        int from = min, to = max;
        if (range != "*") {
            if (const auto dash = range.find('-'); dash != std::string::npos) {
                from = value(range.substr(0, dash));
                to = value(range.substr(dash + 1));
                if (from > to) throw fail("range " + range + " is reversed");
            } else {
                from = value(range);
                to = part.find('/') != std::string::npos ? max : from;  // "5/10" means 5, 15, 25, ...
            }
        }
        for (int v = from; v <= to; v += step) bits.set(static_cast<std::size_t>(v));
    }
    return bits;
}

// 1970-01-01 was a Thursday; 0 = Sunday.
unsigned weekday(const Date& d) {
    const std::int64_t w = (d.toDays() + 4) % 7;
    return static_cast<unsigned>(w < 0 ? w + 7 : w);
}

}  // namespace

// ---- CronExpression ----------------------------------------------------------

CronExpression CronExpression::parse(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> fields;
    for (std::string f; in >> f;) fields.push_back(f);
    if (fields.size() != 5)
        throw std::invalid_argument("cron expression '" + text +
                                    "' needs 5 fields: minute hour day-of-month month day-of-week");

    static const std::vector<std::string> monthNames = {"jan", "feb", "mar", "apr", "may", "jun",
                                                        "jul", "aug", "sep", "oct", "nov", "dec"};
    static const std::vector<std::string> dayNames = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

    CronExpression c;
    c.text_ = fields[0] + " " + fields[1] + " " + fields[2] + " " + fields[3] + " " + fields[4];
    c.minutes_ = parseField<60>(fields[0], "minute", 0, 59);
    c.hours_ = parseField<24>(fields[1], "hour", 0, 23);
    c.daysOfMonth_ = parseField<32>(fields[2], "day-of-month", 1, 31);
    c.months_ = parseField<13>(fields[3], "month", 1, 12, monthNames);
    const auto dow = parseField<8>(fields[4], "day-of-week", 0, 7, dayNames);
    for (std::size_t d = 0; d < 7; ++d) c.daysOfWeek_[d] = dow[d];
    if (dow[7]) c.daysOfWeek_[0] = true;  // 7 is Sunday too
    c.anyDayOfMonth_ = fields[2] == "*";
    c.anyDayOfWeek_ = fields[4] == "*";
    return c;
}

bool CronExpression::matchesDay(const Date& day) const {
    if (!months_[day.month()]) return false;
    const bool dom = daysOfMonth_[day.day()];
    const bool dow = daysOfWeek_[weekday(day)];
    if (anyDayOfMonth_ && anyDayOfWeek_) return true;
    if (anyDayOfMonth_) return dow;
    if (anyDayOfWeek_) return dom;
    return dom || dow;  // classic cron: either restriction qualifies
}

Timestamp CronExpression::nextAfter(Timestamp after, const TimeZone& zone) const {
    // Start at the next whole local minute.
    const DateTimeParts start = zone.toLocal(after);
    std::int64_t day = start.date.toDays();
    unsigned fromMinuteOfDay = start.hour * 60 + start.minute + 1;

    for (int i = 0; i < 366 * 5; ++i, ++day, fromMinuteOfDay = 0) {
        if (fromMinuteOfDay >= 24 * 60) continue;
        const Date date = Date::fromDays(day);
        if (!matchesDay(date)) continue;
        for (unsigned h = fromMinuteOfDay / 60; h < 24; ++h) {
            if (!hours_[h]) continue;
            const unsigned firstMinute = (h == fromMinuteOfDay / 60) ? fromMinuteOfDay % 60 : 0;
            for (unsigned m = firstMinute; m < 60; ++m) {
                if (!minutes_[m]) continue;
                if (auto utc = zone.toUtc({date, h, m, 0, 0}); utc && *utc > after) return *utc;
            }
        }
    }
    throw std::runtime_error("cron expression '" + text_ + "' never matches");
}

// ---- Schedule ----------------------------------------------------------------

Schedule::Schedule(Kind kind, milliseconds interval, CronExpression cron, TimeZone zone)
    : kind_(kind),
      interval_(interval),
      cron_(std::move(cron)),
      zone_(std::move(zone)) {}

Schedule Schedule::every(milliseconds interval) {
    if (interval.count() <= 0) throw std::invalid_argument("interval must be positive");
    return Schedule(Kind::Every, interval, {}, TimeZone::utc());
}

Schedule Schedule::rate(milliseconds interval) {
    if (interval.count() <= 0) throw std::invalid_argument("interval must be positive");
    return Schedule(Kind::Rate, interval, {}, TimeZone::utc());
}

Schedule Schedule::cron(const std::string& expression, TimeZone zone) {
    CronExpression c = CronExpression::parse(expression);
    c.nextAfter(nowUtc(), zone);  // rejects expressions that never match ("0 0 31 2 *")
    return Schedule(Kind::Cron, milliseconds(0), std::move(c), std::move(zone));
}

milliseconds Schedule::parseDuration(const std::string& text) {
    std::size_t digits = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
    const std::string unit = lower(text.substr(digits));
    if (digits == 0 || digits > 9) throw std::invalid_argument("duration '" + text + "' must start with a number");
    const std::int64_t n = std::stoll(text.substr(0, digits));
    if (unit == "ms") return milliseconds(n);
    if (unit == "s") return std::chrono::seconds(n);
    if (unit == "min" || unit == "m") return std::chrono::minutes(n);
    if (unit == "h") return std::chrono::hours(n);
    if (unit == "d") return std::chrono::hours(24 * n);
    throw std::invalid_argument("duration '" + text + "' needs a unit: ms, s, min, h or d");
}

Schedule Schedule::parse(const std::string& text, const TimeZone& zone) {
    std::istringstream in(text);
    std::string kind, rest;
    in >> kind;
    std::getline(in, rest);
    rest.erase(0, rest.find_first_not_of(' '));
    kind = lower(kind);
    try {
        if (kind == "every" || kind == "rate") {
            if (rest.empty() || rest.find(' ') != std::string::npos)
                throw std::invalid_argument("expected one duration, e.g. '" + kind + " 15s'");
            const auto d = parseDuration(rest);
            return kind == "every" ? every(d) : rate(d);
        }
        if (kind == "cron") return cron(rest, zone);
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument("schedule '" + text + "': " + e.what());
    } catch (const std::runtime_error& e) {
        throw std::invalid_argument("schedule '" + text + "': " + e.what());
    }
    throw std::invalid_argument("schedule '" + text + "' must start with 'every', 'rate' or 'cron'");
}

Timestamp Schedule::nextAfter(Timestamp reference) const {
    if (kind_ == Kind::Cron) return cron_.nextAfter(reference, zone_);
    return reference + std::chrono::duration_cast<std::chrono::microseconds>(interval_);
}

std::string Schedule::text() const {
    switch (kind_) {
        case Kind::Every: return "every " + formatDuration(interval_);
        case Kind::Rate: return "rate " + formatDuration(interval_);
        case Kind::Cron: return "cron " + cron_.text() + " (" + zone_.name() + ")";
    }
    return "?";
}

std::string formatDuration(std::chrono::milliseconds d) {
    const auto ms = d.count();
    if (ms % 86'400'000 == 0) return std::to_string(ms / 86'400'000) + "d";
    if (ms % 3'600'000 == 0) return std::to_string(ms / 3'600'000) + "h";
    if (ms % 60'000 == 0) return std::to_string(ms / 60'000) + "min";
    if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
    return std::to_string(ms) + "ms";
}

}  // namespace caelitus::scheduler
