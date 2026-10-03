// Unit tests for core/DateTime and its database mapping.

#include "TestHarness.hpp"

#include "caelitus/core/DateTime.hpp"
#include "caelitus/core/TimeZone.hpp"
#include "caelitus/db/DbValue.hpp"
#include "caelitus/db/Row.hpp"
#include "caelitus/db/detail/SqlDateTime.hpp"
#include "caelitus/json/JsonTypes.hpp"

#include <ctime>

using namespace caelitus;
using namespace caelitus::db;
using namespace std::chrono;

namespace {

Row rowOf(const std::string& value) {
    auto cols = std::make_shared<const ColumnSet>(std::vector<std::string>{"v"});
    return Row(cols, {value});
}

}  // namespace

// ---- Date --------------------------------------------------------------------

TEST(date_validates_calendar) {
    Date(2024, 2, 29);  // leap year
    Date(2000, 2, 29);  // divisible by 400
    CHECK_THROWS_AS(Date(2025, 2, 29), std::invalid_argument);
    CHECK_THROWS_AS(Date(1900, 2, 29), std::invalid_argument);  // divisible by 100
    CHECK_THROWS_AS(Date(2025, 4, 31), std::invalid_argument);
    CHECK_THROWS_AS(Date(2025, 13, 1), std::invalid_argument);
    CHECK_THROWS_AS(Date(2025, 1, 0), std::invalid_argument);
    CHECK_THROWS_AS(Date(10000, 1, 1), std::invalid_argument);
}

TEST(date_day_numbers) {
    CHECK_EQ(Date(1970, 1, 1).toDays(), 0);
    CHECK_EQ(Date(1969, 12, 31).toDays(), -1);
    CHECK_EQ(Date(2000, 3, 1).toDays(), 11017);
    CHECK(Date::fromDays(20363) == Date(2025, 10, 2));
}

TEST(date_round_trips_over_whole_range) {
    const std::int64_t first = Date(0, 1, 1).toDays();
    const std::int64_t last = Date(9999, 12, 31).toDays();
    Date prev = Date::fromDays(first);
    for (std::int64_t d = first + 1; d <= last; ++d) {
        Date cur = Date::fromDays(d);
        if (cur.toDays() != d || !(prev < cur)) throw test::Failure{"mismatch at day " + std::to_string(d)};
        // consecutive days: either next day of month, or 1st of next month/year
        const bool nextDay = cur.day() == prev.day() + 1 && cur.month() == prev.month();
        const bool nextMonth = cur.day() == 1 && prev.day() == daysInMonth(prev.year(), prev.month());
        if (!nextDay && !nextMonth) throw test::Failure{"gap after " + prev.toString()};
        prev = cur;
    }
    CHECK_THROWS_AS(Date::fromDays(last + 1), std::invalid_argument);
}

TEST(date_formats) {
    CHECK_EQ(Date(7, 3, 9).toString(), "0007-03-09");
    CHECK_EQ(Date(2026, 10, 2).toString(), "2026-10-02");
}

// ---- Timestamp ---------------------------------------------------------------

TEST(timestamp_parts_round_trip) {
    DateTimeParts p{Date(2026, 10, 2), 21, 47, 3, 123456};
    Timestamp ts = fromParts(p);
    DateTimeParts back = toParts(ts);
    CHECK(back.date == p.date);
    CHECK_EQ(back.hour, 21u);
    CHECK_EQ(back.minute, 47u);
    CHECK_EQ(back.second, 3u);
    CHECK_EQ(back.microsecond, 123456u);
}

TEST(timestamp_before_epoch) {
    DateTimeParts p = toParts(Timestamp(microseconds(-1)));
    CHECK(p.date == Date(1969, 12, 31));
    CHECK_EQ(p.hour, 23u);
    CHECK_EQ(p.second, 59u);
    CHECK_EQ(p.microsecond, 999999u);
    CHECK_EQ(toIsoString(fromParts({Date(1000, 1, 1)})), "1000-01-01T00:00:00Z");
}

TEST(timestamp_rejects_bad_time_of_day) {
    CHECK_THROWS_AS(fromParts({Date(2026, 1, 1), 24, 0, 0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(fromParts({Date(2026, 1, 1), 0, 60, 0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(fromParts({Date(2026, 1, 1), 0, 0, 0, 1000000}), std::invalid_argument);
}

TEST(timestamp_iso_string) {
    CHECK_EQ(toIsoString(fromParts({Date(2026, 10, 2), 9, 5, 7, 0})), "2026-10-02T09:05:07Z");
    CHECK_EQ(toIsoString(fromParts({Date(2026, 10, 2), 9, 5, 7, 500})), "2026-10-02T09:05:07.000500Z");
}

TEST(now_utc_matches_libc_gmtime) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    DateTimeParts p = toParts(Timestamp(seconds(t)));
    CHECK(p.date == Date(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1), static_cast<unsigned>(tm.tm_mday)));
    CHECK_EQ(p.hour, static_cast<unsigned>(tm.tm_hour));
    CHECK_EQ(p.minute, static_cast<unsigned>(tm.tm_min));
    CHECK(startOfDay(Date::todayUtc()) <= nowUtc());
}

// ---- SQL text ----------------------------------------------------------------

TEST(sql_datetime_parsing) {
    using detail::parseSqlDateTime;
    CHECK_EQ(toIsoString(parseSqlDateTime("2026-10-02 21:47:03")), "2026-10-02T21:47:03Z");
    CHECK_EQ(toIsoString(parseSqlDateTime("2026-10-02 21:47:03.5")), "2026-10-02T21:47:03.500000Z");
    CHECK_EQ(toIsoString(parseSqlDateTime("2026-10-02 21:47:03.123456")), "2026-10-02T21:47:03.123456Z");
    CHECK_EQ(toIsoString(parseSqlDateTime("2026-10-02")), "2026-10-02T00:00:00Z");
    CHECK_EQ(toIsoString(parseSqlDateTime("9999-12-31 23:59:59.999999")), "9999-12-31T23:59:59.999999Z");

    for (const char* bad : {"", "2026-10-02T21:47:03", "2026-13-01 00:00:00", "2026-02-30", "0000-00-00",
                            "0000-00-00 00:00:00", "2026-10-02 24:00:00", "2026-10-02 21:47:03.1234567",
                            "2026-10-02 21:47:03.", "2026-10-02 21:47", "2026-10-02 21:47:03 ", "26-10-02"}) {
        bool threw = false;
        try {
            parseSqlDateTime(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) throw test::Failure{std::string("accepted bad datetime '") + bad + "'"};
    }
}

TEST(sql_date_parsing) {
    CHECK(detail::parseSqlDate("2024-02-29") == Date(2024, 2, 29));
    CHECK_THROWS_AS(detail::parseSqlDate("2024-02-29 00:00:00"), std::invalid_argument);  // would drop time
    CHECK_THROWS_AS(detail::parseSqlDate("0000-00-00"), std::invalid_argument);
}

TEST(sql_formatting_round_trips) {
    Timestamp ts = fromParts({Date(2026, 10, 2), 21, 47, 3, 120});
    CHECK_EQ(detail::formatSqlDateTime(ts), "2026-10-02 21:47:03.000120");
    CHECK(detail::parseSqlDateTime(detail::formatSqlDateTime(ts)) == ts);
    CHECK_EQ(detail::formatSqlDateTime(fromParts({Date(2026, 10, 2), 1, 2, 3, 0})), "2026-10-02 01:02:03");
}

// ---- Row / DbValue -----------------------------------------------------------

TEST(row_reads_dates_and_timestamps) {
    Row row = rowOf("2026-10-02 21:47:03.250000");
    CHECK_EQ(toIsoString(row.get<Timestamp>("v")), "2026-10-02T21:47:03.250000Z");
    auto asSystem = row.get<system_clock::time_point>("v");
    CHECK(time_point_cast<microseconds>(asSystem) == row.get<Timestamp>("v"));
    auto asSeconds = row.get<time_point<system_clock, seconds>>("v");
    CHECK_EQ(toIsoString(asSeconds), "2026-10-02T21:47:03Z");

    CHECK(rowOf("2026-10-02").get<Date>("v") == Date(2026, 10, 2));
    CHECK_THROWS_AS(row.get<Date>("v"), DataMappingError);
    CHECK_THROWS_AS(rowOf("0000-00-00 00:00:00").get<Timestamp>("v"), DataMappingError);
    CHECK_THROWS_AS(rowOf("garbage").get<Timestamp>("v"), DataMappingError);
}

TEST(row_rejects_time_point_overflow) {
    Row row = rowOf("9999-12-31 23:59:59");
    CHECK_EQ(toIsoString(row.get<Timestamp>("v")), "9999-12-31T23:59:59Z");
    // nanosecond system_clock ends in 2262
    using NanoTimePoint = time_point<system_clock, nanoseconds>;
    CHECK_THROWS_AS(row.get<NanoTimePoint>("v"), DataMappingError);
}

TEST(dbvalue_binds_dates_and_time_points) {
    CHECK(std::holds_alternative<Timestamp>(DbValue(system_clock::now()).storage()));
    CHECK(std::holds_alternative<Timestamp>(DbValue(nowUtc()).storage()));
    CHECK(std::holds_alternative<Date>(DbValue(Date(2026, 1, 1)).storage()));
    CHECK(DbValue(std::optional<Timestamp>{}).isNull());

    // floor, not truncation towards zero, for times before 1970
    DbValue v(time_point<system_clock, nanoseconds>(nanoseconds(-1)));
    CHECK(std::get<Timestamp>(v.storage()) == Timestamp(microseconds(-1)));
}

// ---- ISO 8601 / JSON ---------------------------------------------------------

TEST(iso_timestamp_parsing) {
    CHECK_EQ(toIsoString(parseIsoTimestamp("2026-10-02T21:47:03Z")), "2026-10-02T21:47:03Z");
    CHECK_EQ(toIsoString(parseIsoTimestamp("2026-10-02T21:47:03.5Z")), "2026-10-02T21:47:03.500000Z");
    // nanoseconds are floored to microseconds
    CHECK_EQ(toIsoString(parseIsoTimestamp("2026-10-02T21:47:03.123456789Z")), "2026-10-02T21:47:03.123456Z");
    // offsets are converted to UTC, across midnight
    CHECK_EQ(toIsoString(parseIsoTimestamp("2026-10-03T01:30:00+03:00")), "2026-10-02T22:30:00Z");
    CHECK_EQ(toIsoString(parseIsoTimestamp("2026-10-02T22:00:00-05:30")), "2026-10-03T03:30:00Z");

    for (const char* bad : {"2026-10-02T21:47:03", "2026-10-02 21:47:03Z", "2026-10-02T21:47:03.Z",
                            "2026-10-02T21:47:03.1234567890Z", "2026-10-02T21:47:03+0300", "2026-10-02T21:47:03+24:00",
                            "2026-10-02T21:47Z", "2026-02-30T00:00:00Z", "2026-10-02T21:47:03Zjunk", "2026-10-02"}) {
        bool threw = false;
        try {
            parseIsoTimestamp(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) throw test::Failure{std::string("accepted bad timestamp '") + bad + "'"};
    }
}

TEST(iso_round_trips) {
    for (Timestamp ts : {fromParts({Date(1969, 12, 31), 23, 59, 59, 999999}),
                         fromParts({Date(2026, 10, 2), 0, 0, 0, 1}), fromParts({Date(9999, 12, 31), 23, 59, 59, 0})})
        CHECK(parseIsoTimestamp(toIsoString(ts)) == ts);
    CHECK(Date::parse("2024-02-29") == Date(2024, 2, 29));
    CHECK_THROWS_AS(Date::parse("2024-2-29"), std::invalid_argument);
}

TEST(json_serializes_dates_and_timestamps) {
    const Timestamp ts = fromParts({Date(2026, 10, 2), 21, 47, 3, 120000});
    Json j = {{"takenAt", ts}, {"day", Date(2026, 10, 2)}, {"maybe", std::optional<Timestamp>{}}};
    CHECK_EQ(j.dump(), R"({"day":"2026-10-02","maybe":null,"takenAt":"2026-10-02T21:47:03.120000Z"})");

    Json parsed = Json::parse(j.dump());
    CHECK(parsed.at("takenAt").get<Timestamp>() == ts);
    CHECK(parsed.at("day").get<Date>() == Date(2026, 10, 2));
}

TEST(json_rejects_bad_dates) {
    CHECK_THROWS_AS(Json("yesterday").get<Timestamp>(), Json::exception);
    CHECK_THROWS_AS(Json(1700000000).get<Timestamp>(), Json::exception);
    CHECK_THROWS_AS(Json("2025-02-29").get<Date>(), Json::exception);
}

// ---- Time zones --------------------------------------------------------------

TEST(athens_offsets_follow_eu_summer_time) {
    const TimeZone athens = TimeZone::named("Europe/Athens");
    auto at = [](int y, unsigned mo, unsigned d, unsigned h, unsigned mi) {
        return fromParts({Date(y, mo, d), h, mi, 0, 0});
    };
    // 2026: summer time from Sun 29 March 01:00 UTC to Sun 25 October 01:00 UTC.
    CHECK_EQ(athens.offsetAt(at(2026, 3, 29, 0, 59)).count(), 120);
    CHECK_EQ(athens.offsetAt(at(2026, 3, 29, 1, 0)).count(), 180);
    CHECK_EQ(athens.offsetAt(at(2026, 10, 25, 0, 59)).count(), 180);
    CHECK_EQ(athens.offsetAt(at(2026, 10, 25, 1, 0)).count(), 120);
    CHECK_EQ(athens.offsetAt(at(2026, 1, 15, 12, 0)).count(), 120);
    // 2027: 28 March / 31 October
    CHECK_EQ(athens.offsetAt(at(2027, 3, 28, 1, 0)).count(), 180);
    CHECK_EQ(athens.offsetAt(at(2027, 10, 31, 0, 59)).count(), 180);
}

TEST(local_date_crosses_midnight_before_utc) {
    const TimeZone athens = TimeZone::named("Europe/Athens");
    // 21:30 UTC on 1 July is 00:30 on 2 July in Athens (UTC+3).
    CHECK(athens.localDate(fromParts({Date(2026, 7, 1), 21, 30, 0, 0})) == Date(2026, 7, 2));
    CHECK(athens.localDate(fromParts({Date(2026, 7, 1), 20, 59, 0, 0})) == Date(2026, 7, 1));
    // Winter: UTC+2.
    CHECK(athens.localDate(fromParts({Date(2026, 12, 31), 22, 0, 0, 0})) == Date(2027, 1, 1));
    CHECK(TimeZone::utc().localDate(fromParts({Date(2026, 12, 31), 22, 0, 0, 0})) == Date(2026, 12, 31));
    CHECK(TimeZone::named("Europe/London").localDate(fromParts({Date(2026, 7, 1), 23, 30, 0, 0})) == Date(2026, 7, 2));
}

TEST(local_times_convert_to_utc_across_summer_time_changes) {
    const TimeZone athens = TimeZone::named("Europe/Athens");
    auto local = [](unsigned mo, unsigned d, unsigned h, unsigned mi) -> DateTimeParts {
        return {Date(2026, mo, d), h, mi, 0, 0};
    };
    auto utc = [](unsigned mo, unsigned d, unsigned h, unsigned mi) {
        return fromParts({Date(2026, mo, d), h, mi, 0, 0});
    };
    CHECK(athens.toUtc(local(7, 1, 12, 0)) == utc(7, 1, 9, 0));   // summer, UTC+3
    CHECK(athens.toUtc(local(1, 15, 3, 0)) == utc(1, 15, 1, 0));  // winter, UTC+2
    // 29 March: 03:00 local jumps to 04:00, so 03:30 does not exist.
    CHECK(!athens.toUtc(local(3, 29, 3, 30)));
    CHECK(athens.toUtc(local(3, 29, 4, 0)) == utc(3, 29, 1, 0));
    // 25 October: 04:00 local goes back to 03:00, so 03:30 happens twice; the first wins.
    CHECK(athens.toUtc(local(10, 25, 3, 30)) == utc(10, 25, 0, 30));
    // Round trip.
    const Timestamp t = utc(10, 25, 1, 30);
    CHECK(athens.toUtc(athens.toLocal(t)) == utc(10, 25, 0, 30));  // the earlier 03:30
    CHECK(TimeZone::utc().toUtc(local(5, 5, 5, 5)) == utc(5, 5, 5, 5));
}

TEST(unknown_time_zone_is_rejected) {
    CHECK_THROWS_AS(TimeZone::named("America/New_York"), std::invalid_argument);
    CHECK_THROWS_AS(TimeZone::named("Athens"), std::invalid_argument);
    CHECK_EQ(TimeZone::named("UTC").name(), "UTC");
}

int main() { return test::runAll(); }
