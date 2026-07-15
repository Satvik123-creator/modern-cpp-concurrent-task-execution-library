#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_priority.h"
#include "ctee/core/task_result.h"

namespace ctee {

class ThreadPool;

// ---------------------------------------------------------------------------
// Scheduler — time-based task dispatch onto a ThreadPool.
//
// Architecture:
//
//   schedule_now()  ──→  ThreadPool.submit()  (immediate)
//
//   schedule() / schedule_recurring()
//       ↓
//   Min-heap ordered by next_execution time
//       ↓
//   Timer thread (sleeps until earliest deadline)
//       ↓
//   ThreadPool.submit()
//       ↓
//   (if recurring) re-push into heap with next_execution += interval
//
// The timer thread uses condition_variable_any with wait_until so it
// wakes exactly when the next task is due, or earlier if a new task
// with an earlier deadline is inserted.
// ---------------------------------------------------------------------------
class Scheduler {
public:
    explicit Scheduler(ThreadPool& pool);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Submit a task to the pool immediately (bypasses the heap).
    std::string schedule_now(Task task);

    // Schedule a task for future execution.
    std::string schedule(Task task, std::chrono::steady_clock::time_point when);

    // Schedule a task that repeats at a fixed interval.
    // max_repeats == 0 means unlimited.
    // The interval is measured from completion to next start.
    std::string schedule_recurring(Task task, std::chrono::milliseconds interval,
                                   std::size_t max_repeats = 0);

    // Cancel a scheduled task.  If it has already been submitted to the
    // pool, pool.cancel() is also called.
    bool cancel(const std::string& task_id);

    // Change the execution time of a scheduled task.
    // Returns true if the task was found and rescheduled.
    bool reschedule(const std::string& task_id,
                    std::chrono::steady_clock::time_point new_time);

    // Pause the timer thread.  Already-scheduled tasks remain in the
    // heap and will execute when resume() is called.
    void pause();
    void resume();

    // Shut down the scheduler.  Waits for the timer thread to finish.
    void shutdown();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_paused() const noexcept;
    [[nodiscard]] std::size_t scheduled_count() const noexcept;

private:
    // Stored separately from Task so we can re-create fresh Task objects
    // each cycle of a recurring task.  std::function is copyable, which
    // is what makes this work.
    struct ScheduledEntry {
        std::string id;
        std::string name;
        TaskPriority priority;
        std::function<void()> func;
        std::chrono::steady_clock::time_point next_execution;
        std::chrono::milliseconds interval{0};
        std::size_t max_repeats = 0;
        std::size_t repeat_count = 0;
        bool is_recurring = false;
    };

    // Min-heap comparator: earliest next_execution first.
    struct CompareByTime {
        bool operator()(const ScheduledEntry& a, const ScheduledEntry& b) const {
            return a.next_execution > b.next_execution;
        }
    };

    void timer_loop(std::stop_token stop_token);

    ThreadPool& pool_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::jthread timer_thread_;
    std::vector<ScheduledEntry> schedule_heap_;
    std::unordered_set<std::string> cancelled_ids_;
    bool running_;
    bool paused_;
};

}  // namespace ctee
