#include "caelitus/scheduler/Scheduler.hpp"

#include <random>
#include <stdexcept>

namespace caelitus::scheduler {

namespace {

using Clock = std::chrono::system_clock;

std::chrono::microseconds us(std::chrono::milliseconds ms) {
    return std::chrono::duration_cast<std::chrono::microseconds>(ms);
}

std::chrono::microseconds randomUpTo(std::chrono::milliseconds max) {
    if (max.count() <= 0) return std::chrono::microseconds(0);
    static thread_local std::mt19937_64 rng(std::random_device{}());
    return std::chrono::microseconds(std::uniform_int_distribution<std::int64_t>(0, us(max).count())(rng));
}

double millisBetween(Timestamp a, Timestamp b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::string seconds(std::chrono::microseconds d) {
    return fmt::format("{:.1f}s", std::chrono::duration<double>(d).count());
}

}  // namespace

// Everything the scheduler knows about one job. Guarded by Scheduler::mutex_,
// except `spec`, which never changes after add().
struct Scheduler::Job {
    explicit Job(JobSpec s) : spec(std::move(s)), paused(!spec.enabled) {}

    JobSpec spec;
    bool paused;
    bool running = false;
    bool queued = false;
    std::optional<Timestamp> nextRun;  // planned start; unset while paused, queued or running
    int nextAttempt = 1;               // > 1 when the next run is a retry
    Timestamp ratePlanned{};           // Rate schedules: the last regular planned start (keeps the phase)
    Timestamp runningSince{};
    bool timeoutWarned = false;

    std::optional<Timestamp> lastStart, lastEnd;
    std::optional<double> lastDurationMs;
    std::string lastResult = "never";
    std::string lastError;
    std::uint64_t runs = 0, failures = 0;
    int consecutiveFailures = 0;
    log::LogThrottle failureLog;
};

Scheduler::Scheduler(SchedulerConfig config) : config_(config), log_(log::get("scheduler")) {
    if (config_.threads == 0) throw std::invalid_argument("Scheduler: threads must be > 0");
}

Scheduler::~Scheduler() { stop(); }

void Scheduler::add(JobSpec spec) {
    if (spec.name.empty()) throw std::invalid_argument("Scheduler: a job needs a name");
    if (!spec.run) throw std::invalid_argument("Scheduler: job '" + spec.name + "' has no function");
    std::lock_guard<std::mutex> lock(mutex_);
    if (jobs_.count(spec.name)) throw std::invalid_argument("Scheduler: job '" + spec.name + "' added twice");
    const std::string name = spec.name;
    Job& job = *jobs_.emplace(name, std::make_unique<Job>(std::move(spec))).first->second;
    if (started_ && !stopping_) {
        planFirst(job, nowUtc(), true);
        timerCv_.notify_one();
    }
}

bool Scheduler::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.count(name) > 0;
}

void Scheduler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopping_) return;
    started_ = true;
    const Timestamp now = nowUtc();
    std::size_t paused = 0;
    for (auto& [name, job] : jobs_) {
        planFirst(*job, now, true);
        if (job->paused) ++paused;
        log_->debug("Job '{}' ({}){}", name, job->spec.schedule.text(),
                    job->paused ? ": paused" : ": first run at " + toIsoString(*job->nextRun));
    }
    timer_ = std::thread([this] { timerLoop(); });
    for (std::size_t i = 0; i < config_.threads; ++i) workers_.emplace_back([this] { workerLoop(); });
    log_->info("Scheduler started: {} jobs ({} paused), {} worker threads", jobs_.size(), paused, config_.threads);
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        for (const Work& w : queue_) w.job->queued = false;
        queue_.clear();  // queued runs are dropped; running ones finish
    }
    timerCv_.notify_all();
    workCv_.notify_all();
    if (timer_.joinable()) timer_.join();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
    workers_.clear();
    if (started_) log_->info("Scheduler stopped");
}

// ---- planning ----------------------------------------------------------------

void Scheduler::planFirst(Job& job, Timestamp now, bool initial) {
    job.nextAttempt = 1;
    const Schedule& s = job.spec.schedule;
    if (s.kind() == Schedule::Kind::Cron) {
        job.nextRun = s.nextAfter(now) + randomUpTo(job.spec.jitter);
    } else {
        job.ratePlanned = s.nextAfter(now) + randomUpTo(job.spec.jitter);
        job.nextRun = job.ratePlanned;
    }
    if (initial && job.spec.runOnStart) job.nextRun = now + randomUpTo(job.spec.jitter);
    if (job.paused) job.nextRun.reset();
}

Timestamp Scheduler::nextRegular(Job& job, Timestamp now) const {
    const Schedule& s = job.spec.schedule;
    switch (s.kind()) {
        case Schedule::Kind::Every: return s.nextAfter(now);  // `now` is the end of the run
        case Schedule::Kind::Cron: return s.nextAfter(now);
        case Schedule::Kind::Rate: {
            Timestamp next = s.nextAfter(job.ratePlanned);
            if (next <= now) {  // fell behind: skip missed runs instead of catching up in a burst
                const auto step = us(s.interval());
                next += ((now - next) / step + 1) * step;
            }
            job.ratePlanned = next;
            return next;
        }
    }
    return s.nextAfter(now);
}

void Scheduler::enqueue(Job& job, Timestamp planned, int attempt, bool manual) {
    job.queued = true;
    if (!manual) job.nextRun.reset();
    queue_.push_back({&job, planned, attempt, manual});
    workCv_.notify_one();
}

// ---- threads -----------------------------------------------------------------

void Scheduler::timerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        const Timestamp now = nowUtc();
        Timestamp wake = now + std::chrono::hours(1);
        for (auto& [name, jobPtr] : jobs_) {
            Job& job = *jobPtr;
            if (job.nextRun && !job.paused && !job.running && !job.queued) {
                if (*job.nextRun <= now) enqueue(job, *job.nextRun, job.nextAttempt, false);
                else wake = std::min(wake, *job.nextRun);
            }
            if (job.running && job.spec.timeout.count() > 0 && !job.timeoutWarned) {
                const Timestamp deadline = job.runningSince + us(job.spec.timeout);
                if (deadline <= now) {
                    job.timeoutWarned = true;
                    log_->warn("Job '{}' is still running after {} (timeout {})", name, seconds(now - job.runningSince),
                               formatDuration(job.spec.timeout));
                } else {
                    wake = std::min(wake, deadline);
                }
            }
        }
        timerCv_.wait_until(lock, Clock::time_point(wake.time_since_epoch()));
    }
}

void Scheduler::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        workCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        const Work w = queue_.front();
        queue_.pop_front();
        Job& job = *w.job;
        job.queued = false;
        job.running = true;
        job.timeoutWarned = false;
        job.runningSince = nowUtc();
        timerCv_.notify_one();  // to watch the timeout
        lock.unlock();
        execute(job, w.planned, w.attempt, w.manual);
        lock.lock();
    }
}

void Scheduler::execute(Job& job, Timestamp planned, int attempt, bool manual) {
    const std::string& name = job.spec.name;
    const Timestamp start = nowUtc();
    std::string error;
    try {
        JobContext ctx(name, planned, attempt, stopping_);
        job.spec.run(ctx);
    } catch (const std::exception& e) {
        error = e.what();
        if (error.empty()) error = "unknown error";
    } catch (...) {
        error = "non-standard exception";
    }
    const Timestamp end = nowUtc();
    const double ms = millisBetween(start, end);

    std::lock_guard<std::mutex> lock(mutex_);
    job.running = false;
    job.lastStart = start;
    job.lastEnd = end;
    job.lastDurationMs = ms;
    ++job.runs;

    // A manual run keeps the regular plan, unless that came due meanwhile
    // (it was skipped, as the job was busy).
    const bool keepPlan = manual && job.nextRun && *job.nextRun > end;
    const Timestamp plan = keepPlan ? *job.nextRun : Timestamp{};
    Timestamp next;
    if (error.empty()) {
        if (job.consecutiveFailures > 0)
            log_->info("Job '{}' succeeded again after {} failed run(s)", name, job.consecutiveFailures);
        else log_->debug("Job '{}' finished in {:.1f} ms", name, ms);
        job.lastResult = "ok";
        job.lastError.clear();
        job.consecutiveFailures = 0;
        job.failureLog.reset();
        job.nextAttempt = 1;
        next = keepPlan ? plan : nextRegular(job, end);
    } else {
        job.lastResult = "failed";
        job.lastError = error;
        ++job.failures;
        ++job.consecutiveFailures;
        const Timestamp regular = keepPlan ? plan : nextRegular(job, end);
        next = regular;
        job.nextAttempt = 1;
        if (attempt <= job.spec.retryAttempts && !stopping_) {
            const Timestamp retryAt = end + us(job.spec.retryDelay) * (std::int64_t{1} << std::min(attempt - 1, 20));
            if (retryAt < regular) {
                next = retryAt;
                job.nextAttempt = attempt + 1;
            }
        }
        if (auto suppressed = job.failureLog.allow())
            log_->warn("Job '{}' failed ({} in a row): {}; {} at {}{}", name, job.consecutiveFailures, error,
                       job.nextAttempt > 1 ? "retry" : "next run", toIsoString(next),
                       log::suppressedSuffix(*suppressed));
    }
    if (!job.paused && !stopping_) job.nextRun = next;
    else job.nextRun.reset();
    timerCv_.notify_one();
}

// ---- control and status --------------------------------------------------------

Scheduler::RunResult Scheduler::runNow(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) return RunResult::NotFound;
    Job& job = *it->second;
    if (job.running || job.queued) return RunResult::AlreadyRunning;
    log_->info("Job '{}' started by hand", name);
    enqueue(job, nowUtc(), 1, true);
    return RunResult::Started;
}

bool Scheduler::pause(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) return false;
    Job& job = *it->second;
    if (!job.paused) log_->info("Job '{}' paused", name);
    job.paused = true;
    job.nextRun.reset();
    return true;
}

bool Scheduler::resume(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) return false;
    Job& job = *it->second;
    if (!job.paused) return true;
    job.paused = false;
    if (!job.running && !job.queued) planFirst(job, nowUtc(), false);
    log_->info("Job '{}' resumed; next run at {}", name,
               job.nextRun ? toIsoString(*job.nextRun) : "after the current one");
    timerCv_.notify_one();
    return true;
}

JobStatus Scheduler::snapshot(const Job& job) const {
    JobStatus s;
    s.name = job.spec.name;
    s.description = job.spec.description;
    s.schedule = job.spec.schedule.text();
    s.paused = job.paused;
    s.running = job.running || job.queued;
    s.nextRun = job.nextRun;
    s.lastStart = job.lastStart;
    s.lastEnd = job.lastEnd;
    s.lastDurationMs = job.lastDurationMs;
    s.lastResult = job.lastResult;
    s.lastError = job.lastError;
    s.runs = job.runs;
    s.failures = job.failures;
    s.consecutiveFailures = job.consecutiveFailures;
    return s;
}

std::vector<JobStatus> Scheduler::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobStatus> out;
    for (const auto& [name, job] : jobs_) out.push_back(snapshot(*job));
    return out;
}

std::optional<JobStatus> Scheduler::status(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) return std::nullopt;
    return snapshot(*it->second);
}

}  // namespace caelitus::scheduler
