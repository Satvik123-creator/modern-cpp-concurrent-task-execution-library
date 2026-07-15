#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/task_history.h"
#include "ctee/core/thread_pool_config.h"
#include "ctee/core/worker.h"
#include "ctee/core/metrics.h"

namespace ctee {
namespace detail {
    class TaskQueue;
    class Worker;
}

// ---------------------------------------------------------------------------
// Type trait: detect std::packaged_task<T>
// ---------------------------------------------------------------------------
template<typename T>
struct is_packaged_task : std::false_type {};

template<typename R, typename... Args>
struct is_packaged_task<std::packaged_task<R(Args...)>> : std::true_type {};

template<typename T>
inline constexpr bool is_packaged_task_v = is_packaged_task<T>::value;

// ---------------------------------------------------------------------------
// ThreadPool — auto-scaling concurrent task executor.
//
// Config knobs (all in ThreadPoolConfig):
//
//   min_threads      Minimum number of workers.  The pool never shrinks
//                    below this count.
//
//   max_threads      Maximum number of workers.  The pool never grows
//                    above this count.
//
//   scale_up_threshold  Queue depth at which a new worker is spawned.
//                    When queue_.size() >= scale_up_threshold AND
//                    workers_.size() < max_threads, submit() adds a
//                    worker immediately (under the same lock that
//                    pushes the task).
//
//   idle_timeout     (reserved for future per-worker idle timeout)
//
// ── SCALE UP ───────────────────────────────────────────────────────────
//
//   Trigger:  submit() detects queue_.size() >= scale_up_threshold
//             AND workers_.size() < max_threads.
//   Action:   add_worker() under mutex_.
//   When:     On every submit() call — immediate reaction to load.
//
// ── SCALE DOWN ─────────────────────────────────────────────────────────
//
//   Trigger:  on_task_complete() detects queue_.empty()
//             AND workers_.size() > min_threads.
//   Action:   Find one Waiting worker, call request_stop() on it.
//             The worker drains remaining tasks then exits its loop.
//   When:     After each task completes — gradual cooldown.
//   Reaping:  Stopped workers are reaped (joined + erased) in the
//             next submit() call.  This is safe because submit() is
//             called from user threads, never from worker threads,
//             so join() cannot deadlock.
//
// ── RACE CONDITION PREVENTION ──────────────────────────────────────────
//
//   1. All mutations to workers_, queue_, running_, paused_ go through
//      mutex_.  Scaling decisions are always made under this lock.
//
//   2. Worker::should_stop_ is std::atomic<bool>.  request_stop()
//      sets it with memory_order_release; the worker reads it with
//      memory_order_acquire inside cv_.wait()'s predicate (which
//      holds mutex_).  The CV guarantees that a notify + re-check
//      under the lock sees the latest value.
//
//   3. reap_stopped_workers() is ONLY called from submit(), which is
//      only called from user/submitter threads.  It is never called
//      from on_task_complete() (a worker thread).  This guarantees
//      that Worker::~Worker() → join() never blocks on a thread that
//      is itself waiting for mutex_ (which would deadlock).
//
//   4. A worker sets state_ = Stopped BEFORE its run() function
//      returns.  reap_stopped_workers() checks is_stopped() before
//      calling reset().  Therefore by the time join() is called the
//      thread has already exited the loop and is returning.
//
//   5. on_task_complete() calls request_stop() on a different worker
//      (never itself).  request_stop() only touches an atomic + CV
//      notify, both of which are safe under the mutex.
//
//   6. During shutdown, running_ is set to false under mutex_, then
//      CV is notified.  All workers see !pool_running_ && queue_.empty()
//      and break.  The destructor joins all threads.
// ---------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(const ThreadPoolConfig& config = {});
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template<typename F, typename... Args>
        requires std::invocable<F, Args...>
              && (!is_packaged_task_v<std::decay_t<F>>)
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    template<typename R>
    [[nodiscard]] std::future<R> submit(std::packaged_task<R()> task);

    [[nodiscard]] std::future<void> submit(Task task);

    bool cancel(const std::string& task_id);

    void set_completion_handler(const std::string& task_id,
                                std::function<void(TaskStatus)> handler);

    void pause();
    void resume();
    void shutdown();

    [[nodiscard]] ThreadPoolStats stats() const;
    [[nodiscard]] std::vector<WorkerInfo> get_worker_infos() const;
    [[nodiscard]] QueueInfo queue_info() const;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_paused() const noexcept;
    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::shared_ptr<Metrics> metrics() const noexcept;
    [[nodiscard]] std::shared_ptr<TaskHistory> task_history() const noexcept;
    std::vector<std::string> submit_batch(std::vector<Task> tasks);

private:
    void on_task_complete(const std::string& task_id, TaskStatus status,
                          std::chrono::milliseconds wait_time,
                          std::chrono::milliseconds exec_time);
    void add_worker();
    void try_scale_down();
    void reap_stopped_workers();

    ThreadPoolConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    detail::TaskQueue queue_;
    std::vector<std::unique_ptr<detail::Worker>> workers_;
    std::atomic<std::size_t> next_worker_id_{0};

    std::unordered_map<std::string, std::shared_ptr<std::promise<void>>> void_promises_;
    std::unordered_map<std::string, std::function<void()>> cancel_handlers_;
    std::unordered_map<std::string, std::function<void(TaskStatus)>> completion_callbacks_;

    bool running_;
    bool paused_;
    std::size_t stopping_count_{0};

    std::shared_ptr<Metrics> metrics_;
    std::shared_ptr<TaskHistory> history_;

    // Metadata preserved for tasks that support retry.  Keyed by task_id.
    struct RetryInfo {
        std::function<void()> func;
        std::string name;
        TaskPriority priority;
        std::size_t max_retries;
        std::chrono::milliseconds retry_delay;
        std::chrono::milliseconds timeout_ms;
        std::vector<std::string> depends_on;
    };
    std::unordered_map<std::string, RetryInfo> retry_info_;
    std::unordered_map<std::string, std::size_t> retry_counts_;

    std::atomic<std::size_t> total_completed_{0};
    std::atomic<std::size_t> total_failed_{0};
};

// ===========================================================================
// Template implementations
// ===========================================================================

template<typename F, typename... Args>
    requires std::invocable<F, Args...>
          && (!is_packaged_task_v<std::decay_t<F>>)
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using R = std::invoke_result_t<F, Args...>;

    auto promise = std::make_shared<std::promise<R>>();
    auto future = promise->get_future();
    auto once = std::make_shared<std::once_flag>();
    auto setter = std::make_shared<std::function<void()>>([](){});

    Task task([setter, once, promise, fn = std::forward<F>(f),
               ...capture = std::forward<Args>(args)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                std::invoke(std::move(fn), std::move(capture)...);
                *setter = [promise]() { promise->set_value(); };
            } else {
                auto result = std::invoke(std::move(fn), std::move(capture)...);
                auto result_ptr = std::make_shared<R>(std::move(result));
                *setter = [promise, result_ptr]() {
                    promise->set_value(std::move(*result_ptr));
                };
            }
        } catch (...) {
            auto ep = std::current_exception();
            *setter = [promise, ep]() { promise->set_exception(ep); };
        }
    });

    {
        std::lock_guard lock(mutex_);

        reap_stopped_workers();

        if (!running_) {
            try {
                throw std::runtime_error("ThreadPool is shut down");
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            return future;
        }

        if (queue_.is_full()) {
            try {
                throw std::runtime_error("Task queue is full");
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            return future;
        }

        std::string id = task.id();
        cancel_handlers_[id] = [promise, once]() {
            std::call_once(*once, [promise]() {
                try {
                    throw std::runtime_error("Task was cancelled");
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        };
        completion_callbacks_[id] = [setter, once](TaskStatus status) {
            if (status != TaskStatus::Cancelled) {
                std::call_once(*once, [setter]() { (*setter)(); });
            }
        };

        task.set_status(TaskStatus::Queued);
        queue_.push(std::move(task));
        cv_.notify_one();

        metrics_->record_submit();
        metrics_->set_queue_length(queue_.size());

        if (queue_.size() >= config_.scale_up_threshold &&
            workers_.size() < config_.max_threads) {
            add_worker();
        }
    }

    return future;
}

template<typename R>
std::future<R> ThreadPool::submit(std::packaged_task<R()> task) {
    auto pt = std::make_shared<std::packaged_task<R()>>(std::move(task));
    auto future = pt->get_future();

    Task wrapper([pt]() mutable {
        (*pt)();
    });

    {
        std::lock_guard lock(mutex_);

        reap_stopped_workers();

        if (!running_ || queue_.is_full()) {
            return future;
        }

        wrapper.set_status(TaskStatus::Queued);
        queue_.push(std::move(wrapper));
        cv_.notify_one();

        metrics_->record_submit();
        metrics_->set_queue_length(queue_.size());

        if (queue_.size() >= config_.scale_up_threshold &&
            workers_.size() < config_.max_threads) {
            add_worker();
        }
    }

    return future;
}

}  // namespace ctee
