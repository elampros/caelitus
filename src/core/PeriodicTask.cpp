#include "caelitus/core/PeriodicTask.hpp"

#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"

namespace caelitus {

PeriodicTask::PeriodicTask(std::string name, std::chrono::milliseconds interval, std::function<void()> fn)
    : name_(std::move(name)),
      interval_(interval),
      fn_(std::move(fn)) {
    if (interval_.count() <= 0) throw std::invalid_argument("PeriodicTask: interval must be > 0");
    if (!fn_) throw std::invalid_argument("PeriodicTask: function is empty");
}

PeriodicTask::~PeriodicTask() { stop(); }

void PeriodicTask::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
}

void PeriodicTask::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PeriodicTask::loop() {
    auto log = log::get("task");
    log::LogThrottle errors;
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        if (cv_.wait_for(lock, interval_, [this] { return !running_; })) break;
        lock.unlock();
        try {
            fn_();
        } catch (const std::exception& e) {
            if (auto suppressed = errors.allow())
                log->error("Task '{}' failed: {}{}", name_, e.what(), log::suppressedSuffix(*suppressed));
        } catch (...) {
            if (auto suppressed = errors.allow())
                log->error("Task '{}' failed with a non-standard exception{}", name_,
                           log::suppressedSuffix(*suppressed));
        }
        lock.lock();
    }
}

}  // namespace caelitus
