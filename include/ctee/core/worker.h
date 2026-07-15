#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <stop_token>
#include <thread>

#include "ctee/core/task_queue.h"
#include "ctee/core/task_result.h"

namespace ctee::detail {

// ---------------------------------------------------------------------------
// Worker — single task-execution thread owned by a ThreadPool.
//
// Lifecycle:
//   Idle → Waiting → Running → Waiting → ... → Stopped
//
// The worker thread blocks in cv_.wait() until one of:
//   1. A task is available (queue non-empty) AND pool is not paused.
//   2. The pool is shutting down (pool_running_ == false).
//   3. The pool requests this worker to stop (should_stop_ == true).
//
// Scale-down protocol:
//   The ThreadPool sets should_stop_ via request_stop() when it decides
//   this worker is excess (pool has more workers than min_threads and
//   the queue is empty).  The worker continues draining any remaining
//   tasks, then exits its loop when queue_.empty() && should_stop_.
//   This avoids cv_.wait_until which has known issues with stop_token
//   on some platforms (GCC/libstdc++).
// ---------------------------------------------------------------------------
class Worker {
public:
    enum class State : std::uint8_t {
        Idle,
        Waiting,
        Running,
        Stopped
    };

    using TaskCompleteCallback = std::function<void(
        const std::string& task_id,
        TaskStatus status,
        std::chrono::milliseconds wait_time,
        std::chrono::milliseconds exec_time)>;
    using TaskStartCallback = std::function<void()>;

    Worker(std::size_t id,
           std::mutex& mutex,
           std::condition_variable_any& cv,
           TaskQueue& queue,
           bool& pool_running,
           bool& pool_paused,
           TaskCompleteCallback on_complete = nullptr,
           TaskStartCallback on_start = nullptr);

    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    [[nodiscard]] std::size_t id() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool is_stopped() const noexcept;
    [[nodiscard]] bool is_marked_stop() const noexcept;

    void request_stop() noexcept;

    static const char* state_to_string(State state) noexcept;

private:
    void run(std::stop_token stop_token);

    std::size_t id_;
    std::jthread thread_;

    std::mutex& mutex_;
    std::condition_variable_any& cv_;
    TaskQueue& queue_;
    bool& pool_running_;
    bool& pool_paused_;

    TaskCompleteCallback on_complete_;
    TaskStartCallback on_start_;

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> should_stop_{false};
};

}  // namespace ctee::detail
