#include "caelitus/core/TimeZone.hpp"

#include <map>
#include <stdexcept>

namespace caelitus {

namespace {

// 1970-01-01 was a Thursday; 0 = Sunday.
int weekday(std::int64_t days) {
    const std::int64_t w = (days + 4) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

Date lastSunday(int year, unsigned month) {
    const std::int64_t last = Date(year, month, daysInMonth(year, month)).toDays();
    return Date::fromDays(last - weekday(last));
}

}  // namespace

TimeZone TimeZone::utc() { return TimeZone("UTC", std::chrono::minutes(0), false); }

TimeZone TimeZone::named(const std::string& name) {
    // name -> standard offset in minutes (all observe EU summer time)
    static const std::map<std::string, int> eu = {
        {"Europe/London", 0},     {"Europe/Dublin", 0},      {"Europe/Lisbon", 0},     {"Europe/Berlin", 60},
        {"Europe/Paris", 60},     {"Europe/Rome", 60},       {"Europe/Madrid", 60},    {"Europe/Amsterdam", 60},
        {"Europe/Brussels", 60},  {"Europe/Vienna", 60},     {"Europe/Stockholm", 60}, {"Europe/Copenhagen", 60},
        {"Europe/Warsaw", 60},    {"Europe/Prague", 60},     {"Europe/Budapest", 60},  {"Europe/Athens", 120},
        {"Europe/Helsinki", 120}, {"Europe/Bucharest", 120}, {"Europe/Sofia", 120},    {"Europe/Riga", 120},
        {"Europe/Vilnius", 120},  {"Europe/Tallinn", 120},   {"Asia/Nicosia", 120},
    };
    if (name == "UTC" || name == "Etc/UTC") return utc();
    auto it = eu.find(name);
    if (it == eu.end())
        throw std::invalid_argument("Unsupported time zone '" + name + "' (use UTC or a European zone)");
    return TimeZone(name, std::chrono::minutes(it->second), true);
}

std::chrono::minutes TimeZone::offsetAt(Timestamp ts) const {
    if (!euDst_) return standardOffset_;
    const int year = toParts(ts).date.year();
    const Timestamp start = fromParts({lastSunday(year, 3), 1, 0, 0, 0});
    const Timestamp end = fromParts({lastSunday(year, 10), 1, 0, 0, 0});
    return (ts >= start && ts < end) ? standardOffset_ + std::chrono::minutes(60) : standardOffset_;
}

Date TimeZone::localDate(Timestamp ts) const { return toParts(ts + offsetAt(ts)).date; }

}  // namespace caelitus
