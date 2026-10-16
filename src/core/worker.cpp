#include "ctee/core/worker.h"
#include "ctee/core/metrics.h"
#include "ctee/common/thread_naming.h"

#include <future>
#include <sstream>

using namespace std;
namespace ch = std::chrono;

namespace ctee::detail {

Worker::Worker(size_t id,
               mutex& mutex,
               condition_variable_any& cv,
               TaskQueue& queue,
               bool& pool_running,
               bool& pool_paused,
               TaskCompleteCallback on_complete,
               TaskStartCallback on_start)
    : id_(id),
      mutex_(mutex),
      cv_(cv),
      queue_(queue),
      pool_running_(pool_running),
      pool_paused_(pool_paused),
      on_complete_(move(on_complete)),
      on_start_(move(on_start)) {
    thread_ = jthread([this](stop_token token) { run(token); });
}

Worker::~Worker() {
    if (thread_.joinable()) {
        thread_.request_stop();
        cv_.notify_all();
    }
}

std::size_t Worker::id() const noexcept {
    return id_;
}

Worker::State Worker::state() const noexcept {
    return state_.load(std::memory_order_relaxed);
}

bool Worker::is_stopped() const noexcept {
    return state_.load(std::memory_order_relaxed) == State::Stopped;
}

bool Worker::is_marked_stop() const noexcept {
    return should_stop_.load(std::memory_order_relaxed);
}

void Worker::request_stop() noexcept {
    should_stop_.store(true, std::memory_order_release);
    cv_.notify_all();
}

const char* Worker::state_to_string(State state) noexcept {
    switch (state) {
        case State::Idle:     return "idle";
        case State::Waiting:  return "waiting";
        case State::Running:  return "running";
        case State::Stopped:  return "stopped";
    }
    return "unknown";
}

void Worker::run(stop_token stop_token) {
    // Name this thread for debuggers/profilers (e.g. "ctee-w0", "ctee-w1").
    {
        ostringstream oss;
        oss << "ctee-w" << id_;
        ctee::common::set_thread_name(oss.str());
    }

    state_.store(State::Waiting, memory_order_release);

    while (true) {
        optional<Task> task_opt;

        {
            unique_lock lock(mutex_);

            cv_.wait(lock, stop_token, [this] {
                return (!queue_.empty() && !pool_paused_)
                    || !pool_running_
                    || should_stop_.load(memory_order_acquire);
            });

            if (!pool_running_ && queue_.empty()) {
                break;
            }

            if (stop_token.stop_requested() && queue_.empty()) {
                break;
            }

            if (should_stop_.load(memory_order_acquire) && queue_.empty()) {
                break;
            }

            if (pool_paused_ || queue_.empty()) {
                continue;
            }

            task_opt = queue_.pop();
        }

        if (!task_opt) {
            continue;
        }

        state_.store(State::Running, memory_order_release);
        auto start_time = ch::steady_clock::now();
        auto wait_time = ch::duration_cast<ch::milliseconds>(
            start_time - task_opt->created_at());
        if (on_start_) on_start_();

        // Timeout support: run the task on a detached thread and use
        // std::shared_future (NOT std::async) for the wait.
        //
        // DESIGN: std::async's future destructor BLOCKS until the async
        // task completes, which defeats timeout entirely.  std::shared_future
        // does NOT block on destruction, so on timeout the worker moves on
        // immediately while the detached thread continues in the background.
        //
        // The task is moved into a shared_ptr so it stays alive after the
        // worker returns — the detached thread still holds a copy.
        //
        // On completion within timeout, shared_future::wait_for returns
        // 'ready' and there is a happens-before guarantee from
        // promise.set_value() → shared_future becoming ready, so reading
        // the task's status/result is safe.
        //
        // On timeout, we do NOT read the task's fields (the detached thread
        // may be modifying them concurrently).  We just report TimedOut.
        if (task_opt->timeout_ms().count() > 0) {
            auto task_ptr = make_shared<Task>(move(*task_opt));

            shared_future<void> done;
            {
                promise<void> p;
                done = p.get_future().share();
                thread([task_ptr, promise = move(p)]() mutable {
                    task_ptr->execute();
                    promise.set_value();
                }).detach();
            }

            if (done.wait_for(task_ptr->timeout_ms()) == future_status::ready) {
                // Completed within timeout — safe to read because of
                // happens-before: execute() → set_value() → ready.
                if (on_complete_) {
                    on_complete_(task_ptr->id(), task_ptr->status(),
                                 wait_time, task_ptr->result().execution_time);
                }
            } else {
                // Timeout — detached thread is still running execute().
                // Do NOT read task_ptr fields (data race).  Just report.
                if (on_complete_) {
                    on_complete_(task_ptr->id(), TaskStatus::TimedOut,
                                 wait_time, task_ptr->timeout_ms());
                }
            }

            state_.store(State::Waiting, memory_order_release);
            continue;
        }

        task_opt->execute();

        auto exec_time = task_opt->result().execution_time;

        if (on_complete_) {
            on_complete_(task_opt->id(), task_opt->status(), wait_time, exec_time);
        }

        state_.store(State::Waiting, memory_order_release);
    }

    state_.store(State::Stopped, memory_order_release);
}

}  // namespace ctee::detail
