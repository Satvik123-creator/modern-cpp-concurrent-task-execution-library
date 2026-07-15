#include "ctee/core/scheduler.h"
#include "ctee/core/task.h"
#include "ctee/core/thread_pool.h"

namespace ctee {

Scheduler::Scheduler(ThreadPool& pool)
    : pool_(pool), running_(true), paused_(false) {
    timer_thread_ = std::jthread([this](std::stop_token token) { timer_loop(token); });
}

Scheduler::~Scheduler() {
    shutdown();
}

std::string Scheduler::schedule_now(Task task) {
    std::string id = task.id();
    [[maybe_unused]] auto fut = pool_.submit(std::move(task));
    return id;
}

std::string Scheduler::schedule(Task task, std::chrono::steady_clock::time_point when) {
    std::lock_guard lock(mutex_);
    std::string id = task.id();
    ScheduledEntry entry;
    entry.id = id;
    entry.name = task.name();
    entry.priority = task.priority();
    entry.func = task.func();
    entry.next_execution = when;
    entry.is_recurring = false;
    schedule_heap_.push_back(std::move(entry));
    std::push_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
    cv_.notify_one();
    return id;
}

std::string Scheduler::schedule_recurring(Task task, std::chrono::milliseconds interval,
                                          std::size_t max_repeats) {
    std::lock_guard lock(mutex_);
    std::string id = task.id();
    ScheduledEntry entry;
    entry.id = id;
    entry.name = task.name();
    entry.priority = task.priority();
    entry.func = task.func();
    entry.next_execution = std::chrono::steady_clock::now() + interval;
    entry.interval = interval;
    entry.max_repeats = max_repeats;
    entry.repeat_count = 0;
    entry.is_recurring = true;
    schedule_heap_.push_back(std::move(entry));
    std::push_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
    cv_.notify_one();
    return id;
}

bool Scheduler::cancel(const std::string& task_id) {
    std::lock_guard lock(mutex_);
    cancelled_ids_.insert(task_id);
    pool_.cancel(task_id);
    cv_.notify_one();
    return true;
}

bool Scheduler::reschedule(const std::string& task_id,
                           std::chrono::steady_clock::time_point new_time) {
    std::lock_guard lock(mutex_);
    for (auto& entry : schedule_heap_) {
        if (entry.id == task_id) {
            entry.next_execution = new_time;
            std::make_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
            cv_.notify_one();
            return true;
        }
    }
    return false;
}

void Scheduler::pause() {
    std::lock_guard lock(mutex_);
    paused_ = true;
}

void Scheduler::resume() {
    std::lock_guard lock(mutex_);
    paused_ = false;
    cv_.notify_all();
}

void Scheduler::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

bool Scheduler::is_running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

bool Scheduler::is_paused() const noexcept {
    std::lock_guard lock(mutex_);
    return paused_;
}

std::size_t Scheduler::scheduled_count() const noexcept {
    std::lock_guard lock(mutex_);
    return schedule_heap_.size();
}

void Scheduler::timer_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::unique_lock lock(mutex_);

        cv_.wait(lock, stop_token, [&] {
            return !schedule_heap_.empty() || !running_;
        });

        if (!running_) break;
        if (schedule_heap_.empty()) continue;

        while (paused_ && running_) {
            cv_.wait(lock, stop_token, [&] {
                return !paused_ || !running_;
            });
        }
        if (!running_) break;
        if (schedule_heap_.empty()) continue;

        auto deadline = schedule_heap_.front().next_execution;
        cv_.wait_until(lock, stop_token, deadline, [&] {
            return (!schedule_heap_.empty() &&
                    schedule_heap_.front().next_execution < deadline) ||
                   !running_;
        });

        if (!running_) break;
        if (schedule_heap_.empty()) continue;

        auto now = std::chrono::steady_clock::now();
        if (schedule_heap_.front().next_execution > now) continue;

        std::pop_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
        ScheduledEntry entry = std::move(schedule_heap_.back());
        schedule_heap_.pop_back();

        if (cancelled_ids_.count(entry.id)) {
            cancelled_ids_.erase(entry.id);
            continue;
        }

        bool should_reschedule = entry.is_recurring &&
            (entry.max_repeats == 0 || entry.repeat_count + 1 < entry.max_repeats);

        std::string entry_id = entry.id;
        std::string entry_name = entry.name;
        TaskPriority entry_priority = entry.priority;
        auto entry_func = entry.func;
        bool entry_is_recurring = entry.is_recurring;
        auto entry_interval = entry.interval;
        auto entry_max_repeats = entry.max_repeats;
        auto entry_repeat_count = entry.repeat_count;

        lock.unlock();

        if (!paused_) {
            Task t(entry_func, entry_name, entry_priority);
            [[maybe_unused]] auto fut = pool_.submit(std::move(t));
        } else {
            lock.lock();
            entry.next_execution = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
            schedule_heap_.push_back(std::move(entry));
            std::push_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
            lock.unlock();
            continue;
        }

        if (should_reschedule) {
            lock.lock();
            ScheduledEntry next;
            next.id = entry_id;
            next.name = entry_name;
            next.priority = entry_priority;
            next.func = entry_func;
            next.is_recurring = entry_is_recurring;
            next.interval = entry_interval;
            next.max_repeats = entry_max_repeats;
            next.repeat_count = entry_repeat_count + 1;
            next.next_execution = std::chrono::steady_clock::now() + entry_interval;
            schedule_heap_.push_back(std::move(next));
            std::push_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
            lock.unlock();
        }
    }
}

}  // namespace ctee
