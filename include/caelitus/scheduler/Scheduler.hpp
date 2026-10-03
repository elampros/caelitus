#pragma once

/// @file
/// Runs named jobs on schedules, with status, pause/resume and manual runs.
/// @ingroup scheduler

#include "caelitus/core/DateTime.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"
#include "caelitus/scheduler/Schedule.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace caelitus::scheduler {

/// What a running job can ask the scheduler.
class JobContext {
public:
    /// @cond INTERNAL
    JobContext(const std::string& name, Timestamp plannedFor, int attempt, const std::atomic<bool>& stopping)
        : name_(name),
          plannedFor_(plannedFor),
          attempt_(attempt),
          stopping_(stopping) {}
    /// @endcond

    /// The job's name.
    const std::string& name() const noexcept { return name_; }
    /// When this run was planned to start (it may start a little later).
    Timestamp plannedFor() const noexcept { return plannedFor_; }
    /// 1 for a regular run; 2, 3, ... for retries after a failure.
    int attempt() const noexcept { return attempt_; }
    /// True once the scheduler is stopping. Long jobs should check it and
    /// return early, so the application can shut down quickly.
    bool stopRequested() const noexcept { return stopping_.load(); }

private:
    const std::string& name_;
    Timestamp plannedFor_;
    int attempt_;
    const std::atomic<bool>& stopping_;
};

/// The work of a job. Throw (any std::exception) to report a failure; the
/// message is kept in the job's status and logged.
using JobFunction = std::function<void(JobContext&)>;

/// Everything about one job; pass it to Scheduler::add().
struct JobSpec {
    std::string name;         ///< Unique, e.g. "health"; used in the API and the configuration.
    std::string description;  ///< One line for people, shown by scheduler.list.
    Schedule schedule;        ///< When it runs.
    JobFunction run;          ///< What it does.
    /// false: the job starts paused (it can still be run by hand or resumed).
    bool enabled = true;
    /// Run once right after start(), then follow the schedule.
    bool runOnStart = false;
    /// A run taking longer is logged as a warning (it is not interrupted:
    /// C++ cannot safely kill a thread). 0: no limit.
    std::chrono::milliseconds timeout{0};
    /// A random delay of 0..jitter added to the first run, so jobs with the
    /// same schedule do not all start at the same moment.
    std::chrono::milliseconds jitter{0};
    /// After a failure, retry up to this many times before waiting for the
    /// next regular run. 0: no retries.
    int retryAttempts = 0;
    /// Wait before the first retry; it doubles for each further retry (but a
    /// retry never comes later than the next regular run).
    std::chrono::milliseconds retryDelay{10'000};
};

/// A job's state and history, for monitoring.
struct JobStatus {
    std::string name;                      ///< Job name.
    std::string description;               ///< One line for people.
    std::string schedule;                  ///< Canonical text, e.g. "every 15s".
    bool paused = false;                   ///< Paused jobs only run when started by hand.
    bool running = false;                  ///< A run is in progress.
    std::optional<Timestamp> nextRun;      ///< When it runs next; none while paused or running.
    std::optional<Timestamp> lastStart;    ///< Start of the last run.
    std::optional<Timestamp> lastEnd;      ///< End of the last run.
    std::optional<double> lastDurationMs;  ///< Duration of the last run.
    /// "never", "ok" or "failed".
    std::string lastResult = "never";
    std::string lastError;        ///< Message of the last failure; empty after a success.
    std::uint64_t runs = 0;       ///< Completed runs, successful or not.
    std::uint64_t failures = 0;   ///< Failed runs.
    int consecutiveFailures = 0;  ///< Failures since the last success.
};

/// Scheduler settings.
struct SchedulerConfig {
    /// Jobs that can run at the same time (each job never overlaps itself).
    std::size_t threads = 2;
};

/// Runs named jobs on their schedules.
///
/// One timer thread decides what is due; a small pool of worker threads runs
/// the jobs, so a slow job delays nothing else. A job never runs twice at the
/// same time: if it is still running when it is due again, the due run is
/// skipped.
///
/// Reusable anywhere in the application: create one, add() jobs (before or
/// after start()), start() it, and stop() it on shutdown.
///
/// @code
/// scheduler::Scheduler s({/*threads=*/2});
/// s.add({"cleanup", "Delete old rows", Schedule::parse("cron 0 3 * * *", TimeZone::named("Europe/Athens")),
///        [&](JobContext&) { repo->deleteOld(); }});
/// s.start();
/// ...
/// s.pause("cleanup");
/// s.runNow("cleanup");   // runs even while paused
/// for (const JobStatus& j : s.list()) ...;
/// s.stop();              // waits for running jobs
/// @endcode
///
/// Thread-safe. Logs to "scheduler": start/stop, pause/resume/manual runs and
/// recoveries at info; every run at debug; failures and runs exceeding their
/// timeout at warn (failures throttled to one line a minute per job).
class Scheduler {
public:
    /// Outcome of runNow().
    enum class RunResult {
        Started,         ///< The job was queued and starts at once.
        AlreadyRunning,  ///< A run is in progress; no second run was started.
        NotFound         ///< No job with that name.
    };

    /// @throws std::invalid_argument if `threads` is 0.
    explicit Scheduler(SchedulerConfig config = {});
    /// Stops the scheduler.
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Adds a job. Allowed before and after start().
    /// @throws std::invalid_argument for an empty or duplicate name, or no function.
    void add(JobSpec spec);
    /// Whether a job with this name exists.
    bool has(const std::string& name) const;

    /// Starts the timer and worker threads. Starting twice does nothing.
    void start();
    /// Stops planning new runs, tells running jobs (JobContext::stopRequested())
    /// and waits for them to finish. Idempotent; a stopped scheduler cannot be
    /// started again.
    void stop();

    /// Every job's status, by name.
    std::vector<JobStatus> list() const;
    /// One job's status, or std::nullopt if there is no such job.
    std::optional<JobStatus> status(const std::string& name) const;

    /// Runs a job now, paused or not, unless it is already running. The
    /// regular schedule continues afterwards.
    RunResult runNow(const std::string& name);
    /// Stops a job's scheduled runs (a run in progress finishes).
    /// @return false if there is no such job.
    bool pause(const std::string& name);
    /// Resumes a paused job; its next run is planned from now.
    /// @return false if there is no such job.
    bool resume(const std::string& name);

private:
    struct Job;

    void timerLoop();
    void workerLoop();
    void execute(Job& job, Timestamp planned, int attempt, bool manual);
    void planFirst(Job& job, Timestamp now, bool initial);                // requires mutex_
    Timestamp nextRegular(Job& job, Timestamp now) const;                 // requires mutex_
    void enqueue(Job& job, Timestamp planned, int attempt, bool manual);  // requires mutex_
    JobStatus snapshot(const Job& job) const;                             // requires mutex_

    SchedulerConfig config_;
    log::Logger log_;

    mutable std::mutex mutex_;
    std::condition_variable timerCv_;
    std::condition_variable workCv_;
    std::map<std::string, std::unique_ptr<Job>> jobs_;
    struct Work {
        Job* job;
        Timestamp planned;
        int attempt;
        bool manual;
    };
    std::deque<Work> queue_;
    bool started_ = false;
    std::atomic<bool> stopping_{false};
    std::thread timer_;
    std::vector<std::thread> workers_;
};

}  // namespace caelitus::scheduler
