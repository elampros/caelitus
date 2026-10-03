/// @file
/// Tests for schedules, cron expressions and the scheduler. Scheduler tests
/// use short real intervals (tens of milliseconds) and generous margins.
/// @ingroup tests

#include "LogCapture.hpp"
#include "TestHarness.hpp"

#include "caelitus/log/Log.hpp"
#include "caelitus/scheduler/Scheduler.hpp"

#include <atomic>
#include <thread>

using namespace caelitus;
using namespace caelitus::scheduler;
using namespace std::chrono_literals;
using test::CaptureSink;

namespace {

std::shared_ptr<CaptureSink> g_logs = std::make_shared<CaptureSink>();
const TimeZone kAthens = TimeZone::named("Europe/Athens");

Timestamp utc(int y, unsigned mo, unsigned d, unsigned h, unsigned mi) {
    return fromParts({Date(y, mo, d), h, mi, 0, 0});
}

template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

JobSpec job(std::string name, Schedule schedule, JobFunction fn) {
    JobSpec s{std::move(name), "test job", std::move(schedule), std::move(fn)};
    return s;
}

}  // namespace

// ---- Schedule ------------------------------------------------------------------

TEST(schedules_parse_their_text_form) {
    CHECK_EQ(Schedule::parse("every 15s", kAthens).text(), "every 15s");
    CHECK_EQ(Schedule::parse("rate 500ms", kAthens).text(), "rate 500ms");
    CHECK_EQ(Schedule::parse("every 300s", kAthens).text(), "every 5min");
    CHECK_EQ(Schedule::parse("EVERY 2h", kAthens).text(), "every 2h");
    CHECK_EQ(Schedule::parse("cron 0 3 * * *", kAthens).text(), "cron 0 3 * * * (Europe/Athens)");
    CHECK(Schedule::parse("every 1d", kAthens).interval() == std::chrono::hours(24));
    CHECK_THROWS_AS(Schedule::parse("every", kAthens), std::invalid_argument);
    CHECK_THROWS_AS(Schedule::parse("every 15", kAthens), std::invalid_argument);  // no unit
    CHECK_THROWS_AS(Schedule::parse("every 0s", kAthens), std::invalid_argument);  // not positive
    CHECK_THROWS_AS(Schedule::parse("hourly", kAthens), std::invalid_argument);
    CHECK_THROWS_AS(Schedule::parse("cron 0 3 * *", kAthens), std::invalid_argument);  // 4 fields
    CHECK_THROWS_AS(Schedule::parse("cron 60 * * * *", kAthens), std::invalid_argument);
    CHECK_THROWS_AS(Schedule::parse("cron 0 0 31 2 *", kAthens), std::invalid_argument);  // never matches
}

TEST(every_and_rate_add_their_interval) {
    const Timestamp t = utc(2026, 5, 1, 12, 0);
    CHECK(Schedule::every(15s).nextAfter(t) == t + 15s);
    CHECK(Schedule::rate(250ms).nextAfter(t) == t + 250ms);
}

TEST(cron_finds_the_next_local_time) {
    const auto daily3 = CronExpression::parse("0 3 * * *");
    // 2026-07-01 10:00 UTC is 13:00 in Athens; next 03:00 local is 2 July 00:00 UTC.
    CHECK(daily3.nextAfter(utc(2026, 7, 1, 10, 0), kAthens) == utc(2026, 7, 2, 0, 0));
    // Exactly at a match: strictly after, so the next day.
    CHECK(daily3.nextAfter(utc(2026, 7, 2, 0, 0), kAthens) == utc(2026, 7, 3, 0, 0));
    // Winter: UTC+2.
    CHECK(daily3.nextAfter(utc(2026, 12, 1, 10, 0), kAthens) == utc(2026, 12, 2, 1, 0));

    const auto quarter = CronExpression::parse("*/15 * * * *");
    CHECK(quarter.nextAfter(utc(2026, 7, 1, 10, 7), TimeZone::utc()) == utc(2026, 7, 1, 10, 15));
    CHECK(quarter.nextAfter(utc(2026, 7, 1, 10, 45), TimeZone::utc()) == utc(2026, 7, 1, 11, 0));

    // Weekdays at 08:30 UTC; 2026-10-03 is a Saturday.
    const auto weekdays = CronExpression::parse("30 8 * * mon-fri");
    CHECK(weekdays.nextAfter(utc(2026, 10, 3, 9, 0), TimeZone::utc()) == utc(2026, 10, 5, 8, 30));
    // Lists, ranges with steps, month names.
    CHECK(CronExpression::parse("0 0 1 jan,jul *").nextAfter(utc(2026, 2, 1, 0, 0), TimeZone::utc()) ==
          utc(2026, 7, 1, 0, 0));
    CHECK(CronExpression::parse("0 9-17/4 * * *").nextAfter(utc(2026, 1, 1, 9, 30), TimeZone::utc()) ==
          utc(2026, 1, 1, 13, 0));
    // Sunday as 7.
    CHECK(CronExpression::parse("0 12 * * 7").nextAfter(utc(2026, 10, 3, 0, 0), TimeZone::utc()) ==
          utc(2026, 10, 4, 12, 0));
}

TEST(cron_day_of_month_or_day_of_week) {
    // Both restricted: the 13th OR any Friday. 2026-11-06 is a Friday, before the 13th.
    const auto c = CronExpression::parse("0 0 13 * fri");
    CHECK(c.nextAfter(utc(2026, 11, 1, 0, 0), TimeZone::utc()) == utc(2026, 11, 6, 0, 0));
    CHECK(c.nextAfter(utc(2026, 11, 12, 0, 0), TimeZone::utc()) == utc(2026, 11, 13, 0, 0));
}

TEST(cron_across_summer_time_changes) {
    // 29 March 2026: local 03:00-04:00 does not exist in Athens; a 03:30 job skips that day.
    const auto c = CronExpression::parse("30 3 * * *");
    CHECK(c.nextAfter(utc(2026, 3, 28, 12, 0), kAthens) == utc(2026, 3, 30, 0, 30));
    // 25 October 2026: local 03:30 happens twice; the job runs once, the first time.
    const Timestamp first = c.nextAfter(utc(2026, 10, 24, 12, 0), kAthens);
    CHECK(first == utc(2026, 10, 25, 0, 30));
    CHECK(c.nextAfter(first, kAthens) == utc(2026, 10, 26, 1, 30));
}

TEST(cron_rejects_bad_fields) {
    CHECK_THROWS_AS(CronExpression::parse("a * * * *"), std::invalid_argument);
    CHECK_THROWS_AS(CronExpression::parse("* 24 * * *"), std::invalid_argument);
    CHECK_THROWS_AS(CronExpression::parse("* * 0 * *"), std::invalid_argument);
    CHECK_THROWS_AS(CronExpression::parse("* * * 13 *"), std::invalid_argument);
    CHECK_THROWS_AS(CronExpression::parse("*/0 * * * *"), std::invalid_argument);
    CHECK_THROWS_AS(CronExpression::parse("5-1 * * * *"), std::invalid_argument);
    CHECK_THROWS_AS(CronExpression::parse("* * * * funday"), std::invalid_argument);
}

// ---- Scheduler -----------------------------------------------------------------

TEST(jobs_run_repeatedly_on_their_schedule) {
    Scheduler s;
    std::atomic<int> every{0}, rate{0};
    s.add(job("every", Schedule::every(20ms), [&](JobContext&) { ++every; }));
    s.add(job("rate", Schedule::rate(20ms), [&](JobContext&) { ++rate; }));
    s.start();
    CHECK(eventually([&] { return every >= 3 && rate >= 3; }));
    s.stop();
    auto st = s.status("every");
    CHECK(st && st->runs >= 3 && st->lastResult == "ok" && st->lastDurationMs);
    CHECK(s.list().size() == 2u);
}

TEST(a_job_never_overlaps_itself) {
    Scheduler s({4});
    std::atomic<int> active{0}, maxActive{0}, runs{0};
    s.add(job("slow", Schedule::rate(5ms), [&](JobContext&) {
        const int now = ++active;
        maxActive = std::max(maxActive.load(), now);
        std::this_thread::sleep_for(30ms);
        --active;
        ++runs;
    }));
    s.start();
    CHECK(eventually([&] { return runs >= 3; }));
    CHECK(s.runNow("slow") != Scheduler::RunResult::NotFound);  // during a run: AlreadyRunning or a later start
    s.stop();
    CHECK_EQ(maxActive.load(), 1);
}

TEST(run_on_start_and_manual_runs) {
    Scheduler s;
    std::atomic<int> runs{0};
    JobSpec spec = job("daily", Schedule::cron("0 3 * * *", kAthens), [&](JobContext&) { ++runs; });
    spec.runOnStart = true;
    s.add(spec);
    s.start();
    CHECK(eventually([&] { return runs == 1; }));
    CHECK(eventually([&] { return s.status("daily")->nextRun.has_value(); }));
    CHECK(s.status("daily")->nextRun > nowUtc() + std::chrono::minutes(1));  // back to the cron plan
    CHECK(s.runNow("daily") == Scheduler::RunResult::Started);
    CHECK(eventually([&] { return runs == 2; }));
    CHECK(s.runNow("nope") == Scheduler::RunResult::NotFound);
    s.stop();
}

TEST(pause_and_resume) {
    Scheduler s;
    std::atomic<int> runs{0};
    JobSpec spec = job("tick", Schedule::every(10ms), [&](JobContext&) { ++runs; });
    spec.enabled = false;  // starts paused
    s.add(spec);
    s.start();
    std::this_thread::sleep_for(80ms);
    CHECK_EQ(runs.load(), 0);
    CHECK(s.status("tick")->paused && !s.status("tick")->nextRun);
    CHECK(s.runNow("tick") == Scheduler::RunResult::Started);  // manual runs work while paused
    CHECK(eventually([&] { return runs == 1; }));
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(runs.load(), 1);
    CHECK(s.resume("tick"));
    CHECK(eventually([&] { return runs >= 3; }));
    CHECK(s.pause("tick"));
    std::this_thread::sleep_for(30ms);  // a run in progress may finish
    const int afterPause = runs;
    std::this_thread::sleep_for(80ms);
    CHECK_EQ(runs.load(), afterPause);
    CHECK(!s.pause("nope") && !s.resume("nope"));
    s.stop();
}

TEST(failures_are_recorded_retried_and_logged_once) {
    g_logs->clear();
    Scheduler s;
    std::atomic<int> attempts{0};
    JobSpec spec = job("flaky", Schedule::every(std::chrono::hours(1)), [&](JobContext& ctx) {
        ++attempts;
        if (ctx.attempt() < 3) throw std::runtime_error("database down");
    });
    spec.runOnStart = true;
    spec.retryAttempts = 3;
    spec.retryDelay = 10ms;
    s.add(spec);
    s.start();
    CHECK(eventually([&] { return attempts == 3; }));  // run, retry, retry -> success
    CHECK(eventually([&] { return s.status("flaky")->lastResult == "ok"; }));
    const auto st = *s.status("flaky");
    CHECK_EQ(st.failures, 2u);
    CHECK_EQ(st.consecutiveFailures, 0);
    CHECK(st.lastError.empty());
    CHECK_EQ(g_logs->count(spdlog::level::warn, "Job 'flaky' failed"), 1u);  // throttled
    CHECK(g_logs->contains(spdlog::level::info, "succeeded again after 2 failed run(s)"));
    s.stop();
}

TEST(failure_without_retries_waits_for_the_next_run) {
    Scheduler s;
    JobSpec spec =
        job("bad", Schedule::every(std::chrono::hours(1)), [](JobContext&) { throw std::runtime_error("boom"); });
    spec.runOnStart = true;
    s.add(spec);
    s.start();
    CHECK(eventually([&] { return s.status("bad")->lastResult == "failed"; }));
    const auto st = *s.status("bad");
    CHECK_EQ(st.lastError, "boom");
    CHECK_EQ(st.consecutiveFailures, 1);
    CHECK(st.nextRun > nowUtc() + std::chrono::minutes(59));
    s.stop();
}

TEST(timeouts_are_reported) {
    g_logs->clear();
    Scheduler s;
    JobSpec spec =
        job("long", Schedule::every(std::chrono::hours(1)), [](JobContext&) { std::this_thread::sleep_for(150ms); });
    spec.runOnStart = true;
    spec.timeout = 30ms;
    s.add(spec);
    s.start();
    CHECK(eventually([&] { return g_logs->contains(spdlog::level::warn, "Job 'long' is still running"); }));
    s.stop();
}

TEST(stop_tells_running_jobs_and_waits) {
    Scheduler s;
    std::atomic<bool> sawStop{false}, finished{false};
    JobSpec spec = job("worker", Schedule::every(std::chrono::hours(1)), [&](JobContext& ctx) {
        while (!ctx.stopRequested()) std::this_thread::sleep_for(2ms);
        sawStop = true;
        std::this_thread::sleep_for(20ms);
        finished = true;
    });
    spec.runOnStart = true;
    s.add(spec);
    s.start();
    CHECK(eventually([&] { return s.status("worker")->running; }));
    s.stop();
    CHECK(sawStop && finished);  // stop() returned only after the job ended
    s.stop();                    // idempotent
}

TEST(jobs_can_be_added_after_start) {
    Scheduler s;
    s.start();
    std::atomic<int> runs{0};
    s.add(job("late", Schedule::every(10ms), [&](JobContext&) { ++runs; }));
    CHECK(eventually([&] { return runs >= 2; }));
    CHECK_THROWS_AS(s.add(job("late", Schedule::every(10ms), [](JobContext&) {})), std::invalid_argument);
    CHECK_THROWS_AS(s.add(job("", Schedule::every(10ms), [](JobContext&) {})), std::invalid_argument);
    CHECK_THROWS_AS(s.add(job("nofn", Schedule::every(10ms), nullptr)), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({0}), std::invalid_argument);
    s.stop();
}

/// Runs every test case of this file (see TestHarness.hpp).
int main() {
    caelitus::log::LogConfig config;
    config.level = "debug";
    config.console = false;
    config.extraSinks = {g_logs};
    caelitus::log::init(config);
    return test::runAll();
}
