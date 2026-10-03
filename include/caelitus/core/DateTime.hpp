#pragma once

/// @file
/// Timestamps, calendar dates and their ISO 8601 text form.
/// @ingroup core

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace caelitus {

/// A point in time, always UTC, with microsecond precision (the finest the
/// database stores).
///
/// Microseconds give a range of +-292,000 years, so every DATETIME value fits
/// (a nanosecond system_clock would overflow past 2262).
using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/// The current time, truncated to microseconds.
inline Timestamp nowUtc() {
    return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
}

/// A calendar date with no time of day and no time zone (SQL DATE), years 0..9999.
///
/// Always valid: every constructor rejects impossible dates. Compares
/// chronologically.
class Date {
public:
    /// @throws std::invalid_argument for an impossible date (e.g. 2025-02-29)
    ///         or a year outside 0..9999.
    Date(int year, unsigned month, unsigned day);

    /// The date `days` days after 1970-01-01 (before it, for negative values).
    /// @throws std::invalid_argument if the result is outside years 0..9999.
    static Date fromDays(std::int64_t days);
    /// Today's date in UTC.
    static Date todayUtc();

    int year() const noexcept { return year_; }         ///< 0..9999.
    unsigned month() const noexcept { return month_; }  ///< 1..12.
    unsigned day() const noexcept { return day_; }      ///< 1..31.

    /// Days since 1970-01-01 (negative before it); the inverse of fromDays().
    std::int64_t toDays() const noexcept;

    /// "YYYY-MM-DD".
    std::string toString() const;
    /// Parses "YYYY-MM-DD".
    /// @throws std::invalid_argument for any other format or an impossible date.
    static Date parse(std::string_view text);

    /// @name Comparison
    /// @{
    friend bool operator==(const Date& a, const Date& b) noexcept { return a.toDays() == b.toDays(); }
    friend bool operator!=(const Date& a, const Date& b) noexcept { return !(a == b); }
    friend bool operator<(const Date& a, const Date& b) noexcept { return a.toDays() < b.toDays(); }
    friend bool operator<=(const Date& a, const Date& b) noexcept { return !(b < a); }
    friend bool operator>(const Date& a, const Date& b) noexcept { return b < a; }
    friend bool operator>=(const Date& a, const Date& b) noexcept { return !(a < b); }
    /// @}

private:
    int year_;
    unsigned month_;
    unsigned day_;
};

/// Gregorian leap year rule.
bool isLeapYear(int year) noexcept;
/// 28..31; 0 for a month outside 1..12.
unsigned daysInMonth(int year, unsigned month) noexcept;

/// A Timestamp broken into UTC calendar fields.
struct DateTimeParts {
    Date date{1970, 1, 1};          ///< The calendar day.
    unsigned hour = 0;              ///< 0..23.
    unsigned minute = 0;            ///< 0..59.
    unsigned second = 0;            ///< 0..59 (no leap seconds).
    std::uint32_t microsecond = 0;  ///< 0..999999.
};

/// Splits a timestamp into UTC calendar fields.
DateTimeParts toParts(Timestamp ts);
/// Joins UTC calendar fields into a timestamp; the inverse of toParts().
/// @throws std::invalid_argument if a time field is out of range.
Timestamp fromParts(const DateTimeParts& parts);

/// Midnight UTC of the given date.
inline Timestamp startOfDay(const Date& date) { return fromParts({date}); }

/// Formats as "2026-10-02T21:47:03Z", or "2026-10-02T21:47:03.123456Z" when
/// there are fractions of a second.
std::string toIsoString(Timestamp ts);

/// Parses an RFC 3339 / ISO 8601 timestamp.
///
/// Accepts "YYYY-MM-DDTHH:MM:SS[.fraction]" followed by "Z" or a
/// "+HH:MM"/"-HH:MM" offset, which is converted to UTC. Up to 9 fractional
/// digits (floored to microseconds). A time without a zone is rejected, since
/// it is ambiguous.
/// @throws std::invalid_argument for anything else.
Timestamp parseIsoTimestamp(std::string_view text);

}  // namespace caelitus
