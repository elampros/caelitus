#include "caelitus/db/detail/SqlDateTime.hpp"

#include <cstdio>
#include <stdexcept>

namespace caelitus::db::detail {

namespace {

// Reads exactly `count` digits at `pos`, advancing it.
unsigned readDigits(std::string_view text, std::size_t& pos, std::size_t count) {
    if (pos + count > text.size()) throw std::invalid_argument("truncated");
    unsigned value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') throw std::invalid_argument("expected digit");
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    pos += count;
    return value;
}

void expect(std::string_view text, std::size_t& pos, char c) {
    if (pos >= text.size() || text[pos] != c) throw std::invalid_argument(std::string("expected '") + c + "'");
    ++pos;
}

Date readDate(std::string_view text, std::size_t& pos) {
    const unsigned year = readDigits(text, pos, 4);
    expect(text, pos, '-');
    const unsigned month = readDigits(text, pos, 2);
    expect(text, pos, '-');
    const unsigned day = readDigits(text, pos, 2);
    if (month == 0 || day == 0) throw std::invalid_argument("zero date (or zero month/day) is not a valid date");
    return Date(static_cast<int>(year), month, day);
}

[[noreturn]] void rethrowWithText(const char* what, std::string_view text, const std::exception& e) {
    throw std::invalid_argument(std::string("invalid ") + what + " '" + std::string(text) + "': " + e.what());
}

}  // namespace

std::string formatSqlDate(const Date& date) { return date.toString(); }

std::string formatSqlDateTime(Timestamp ts) {
    const DateTimeParts p = toParts(ts);
    char buf[40];
    if (p.microsecond == 0)
        std::snprintf(buf, sizeof buf, "%s %02u:%02u:%02u", p.date.toString().c_str(), p.hour, p.minute, p.second);
    else
        std::snprintf(buf, sizeof buf, "%s %02u:%02u:%02u.%06u", p.date.toString().c_str(), p.hour, p.minute, p.second,
                      static_cast<unsigned>(p.microsecond));
    return buf;
}

Date parseSqlDate(std::string_view text) {
    try {
        std::size_t pos = 0;
        Date date = readDate(text, pos);
        if (pos != text.size()) throw std::invalid_argument("unexpected trailing characters");
        return date;
    } catch (const std::exception& e) {
        rethrowWithText("date", text, e);
    }
}

Timestamp parseSqlDateTime(std::string_view text) {
    try {
        std::size_t pos = 0;
        DateTimeParts parts;
        parts.date = readDate(text, pos);
        if (pos < text.size()) {
            expect(text, pos, ' ');
            parts.hour = readDigits(text, pos, 2);
            expect(text, pos, ':');
            parts.minute = readDigits(text, pos, 2);
            expect(text, pos, ':');
            parts.second = readDigits(text, pos, 2);
            if (pos < text.size()) {
                expect(text, pos, '.');
                const std::size_t digits = text.size() - pos;
                if (digits < 1 || digits > 6) throw std::invalid_argument("expected 1-6 fractional digits");
                unsigned frac = readDigits(text, pos, digits);
                for (std::size_t i = digits; i < 6; ++i) frac *= 10;
                parts.microsecond = frac;
            }
        }
        return fromParts(parts);
    } catch (const std::exception& e) {
        rethrowWithText("datetime", text, e);
    }
}

}  // namespace caelitus::db::detail
