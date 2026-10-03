#include "caelitus/core/DateTime.hpp"

#include <cstdio>
#include <stdexcept>

namespace caelitus {

namespace {

constexpr std::int64_t kMicrosPerSecond = 1'000'000;
constexpr std::int64_t kMicrosPerDay = 86'400 * kMicrosPerSecond;

// Floor division (rounds towards -infinity), for timestamps before 1970.
std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// Howard Hinnant's public-domain civil calendar algorithms
// (http://howardhinnant.github.io/date_algorithms.html).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Civil {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

Civil civilFromDays(std::int64_t z) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {y + (m <= 2), m, d};
}

class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    unsigned digits(std::size_t count) {
        if (pos_ + count > text_.size()) throw std::invalid_argument("truncated");
        unsigned value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text_[pos_ + i];
            if (c < '0' || c > '9') throw std::invalid_argument("expected digit");
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        pos_ += count;
        return value;
    }

    void expect(char c) {
        if (!consume(c)) throw std::invalid_argument(std::string("expected '") + c + "'");
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool atDigit() const { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; }
    bool done() const { return pos_ == text_.size(); }

    Date date() {
        const unsigned year = digits(4);
        expect('-');
        const unsigned month = digits(2);
        expect('-');
        const unsigned day = digits(2);
        return Date(static_cast<int>(year), month, day);
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

[[noreturn]] void invalid(const char* what, std::string_view text, const std::exception& e) {
    throw std::invalid_argument(std::string("invalid ") + what + " '" + std::string(text) + "': " + e.what());
}

}  // namespace

bool isLeapYear(int year) noexcept { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

unsigned daysInMonth(int year, unsigned month) noexcept {
    static constexpr unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return month == 2 && isLeapYear(year) ? 29 : days[month - 1];
}

// ---- Date --------------------------------------------------------------------

Date::Date(int year, unsigned month, unsigned day) : year_(year), month_(month), day_(day) {
    if (year < 0 || year > 9999) throw std::invalid_argument("Date: year " + std::to_string(year) + " out of range");
    if (month < 1 || month > 12) throw std::invalid_argument("Date: invalid month " + std::to_string(month));
    if (day < 1 || day > daysInMonth(year, month))
        throw std::invalid_argument("Date: invalid day " + std::to_string(year) + "-" + std::to_string(month) + "-" +
                                    std::to_string(day));
}

Date Date::fromDays(std::int64_t days) {
    const Civil c = civilFromDays(days);
    if (c.year < 0 || c.year > 9999) throw std::invalid_argument("Date: day number out of range");
    return Date(static_cast<int>(c.year), c.month, c.day);
}

Date Date::todayUtc() { return toParts(nowUtc()).date; }

std::int64_t Date::toDays() const noexcept { return daysFromCivil(year_, month_, day_); }

std::string Date::toString() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", year_, month_, day_);
    return buf;
}

Date Date::parse(std::string_view text) {
    try {
        Cursor in(text);
        Date d = in.date();
        if (!in.done()) throw std::invalid_argument("unexpected trailing characters");
        return d;
    } catch (const std::exception& e) {
        invalid("date", text, e);
    }
}

// ---- Timestamp ---------------------------------------------------------------

DateTimeParts toParts(Timestamp ts) {
    const std::int64_t us = ts.time_since_epoch().count();
    const std::int64_t days = floorDiv(us, kMicrosPerDay);
    std::int64_t rest = us - days * kMicrosPerDay;  // 0 .. kMicrosPerDay-1

    DateTimeParts p;
    p.date = Date::fromDays(days);
    p.hour = static_cast<unsigned>(rest / (3600 * kMicrosPerSecond));
    rest %= 3600 * kMicrosPerSecond;
    p.minute = static_cast<unsigned>(rest / (60 * kMicrosPerSecond));
    rest %= 60 * kMicrosPerSecond;
    p.second = static_cast<unsigned>(rest / kMicrosPerSecond);
    p.microsecond = static_cast<std::uint32_t>(rest % kMicrosPerSecond);
    return p;
}

Timestamp fromParts(const DateTimeParts& p) {
    if (p.hour > 23 || p.minute > 59 || p.second > 59 || p.microsecond > 999'999)
        throw std::invalid_argument("Timestamp: time of day out of range");
    const std::int64_t us =
        p.date.toDays() * kMicrosPerDay + ((p.hour * 60 + p.minute) * 60 + p.second) * kMicrosPerSecond + p.microsecond;
    return Timestamp(std::chrono::microseconds(us));
}

std::string toIsoString(Timestamp ts) {
    const DateTimeParts p = toParts(ts);
    char buf[40];
    if (p.microsecond == 0)
        std::snprintf(buf, sizeof buf, "%sT%02u:%02u:%02uZ", p.date.toString().c_str(), p.hour, p.minute, p.second);
    else
        std::snprintf(buf, sizeof buf, "%sT%02u:%02u:%02u.%06uZ", p.date.toString().c_str(), p.hour, p.minute, p.second,
                      static_cast<unsigned>(p.microsecond));
    return buf;
}

Timestamp parseIsoTimestamp(std::string_view text) {
    try {
        Cursor in(text);
        DateTimeParts p;
        p.date = in.date();
        in.expect('T');
        p.hour = in.digits(2);
        in.expect(':');
        p.minute = in.digits(2);
        in.expect(':');
        p.second = in.digits(2);
        if (in.consume('.')) {
            std::uint32_t micros = 0;
            int count = 0;
            while (in.atDigit()) {
                const unsigned digit = in.digits(1);
                if (count < 6) micros = micros * 10 + digit;  // digits beyond microseconds are floored away
                ++count;
            }
            if (count < 1 || count > 9) throw std::invalid_argument("expected 1-9 fractional digits");
            for (int i = count; i < 6; ++i) micros *= 10;
            p.microsecond = micros;
        }

        std::int64_t offsetMinutes = 0;
        if (!in.consume('Z')) {
            int sign;
            if (in.consume('+')) sign = 1;
            else if (in.consume('-')) sign = -1;
            else throw std::invalid_argument("expected 'Z' or a +HH:MM / -HH:MM offset");
            const unsigned hours = in.digits(2);
            in.expect(':');
            const unsigned minutes = in.digits(2);
            if (hours > 23 || minutes > 59) throw std::invalid_argument("offset out of range");
            offsetMinutes = sign * static_cast<std::int64_t>(hours * 60 + minutes);
        }
        if (!in.done()) throw std::invalid_argument("unexpected trailing characters");

        // Local time = UTC + offset, so UTC = local - offset.
        return fromParts(p) - std::chrono::minutes(offsetMinutes);
    } catch (const std::exception& e) {
        invalid("timestamp", text, e);
    }
}

}  // namespace caelitus
