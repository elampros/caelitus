#pragma once

/// @file
/// A function run on a schedule.
/// @ingroup core

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace caelitus {

/// Runs a function every `interval` on its own thread until stopped.
///
/// Exceptions from the function are logged (logger "task") and the schedule
/// continues. The first run happens one interval after start().
///
/// @code
/// PeriodicTask flusher("reaction flush", std::chrono::seconds(1), [&] { reactions->flush(); });
/// flusher.start();
/// ...
/// flusher.stop();  // waits for a run in progress
/// @endcode
class PeriodicTask {
public:
    /// @param name      Used in log messages.
    /// @param interval  Time between the end of one run and the start of the next.
    /// @param fn        The work; called on the task's thread.
    PeriodicTask(std::string name, std::chrono::milliseconds interval, std::function<void()> fn);
    /// Stops the task.
    ~PeriodicTask();

    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    /// Starts the thread. Calling it again while running does nothing.
    void start();
    /// Stops the schedule and waits for a run in progress to finish. Idempotent.
    void stop();

private:
    void loop();

    std::string name_;
    std::chrono::milliseconds interval_;
    std::function<void()> fn_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    std::thread thread_;
};

}  // namespace caelitus
