#include "ctee/core/thread_pool.h"

#include <stdexcept>

namespace ctee {

ThreadPool::ThreadPool(const ThreadPoolConfig& config)
    : config_(config),
      queue_(config.max_queue_size),
      running_(true),
      paused_(false),
      metrics_(std::make_shared<Metrics>()),
      history_(std::make_shared<TaskHistory>()) {
    workers_.reserve(config_.min_threads);
    for (std::size_t i = 0; i < config_.min_threads; ++i) {
        add_worker();
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

std::future<void> ThreadPool::submit(Task task) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

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
    void_promises_[id] = promise;
    cancel_handlers_[id] = [promise]() {
        try {
            throw std::runtime_error("Task was cancelled");
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };

    // Store retry metadata so on_task_complete can re-enqueue on failure.
    if (task.max_retries() > 0) {
        RetryInfo info;
        info.func = task.func();
        info.name = task.name();
        info.priority = task.priority();
        info.max_retries = task.max_retries();
        info.retry_delay = task.retry_delay();
        info.timeout_ms = task.timeout_ms();
        info.depends_on = task.depends_on();
        retry_info_[id] = std::move(info);
    }

    history_->record_submit(id, task.name(), task.created_at());

    task.set_status(TaskStatus::Queued);
    queue_.push(std::move(task));
    cv_.notify_one();

    metrics_->record_submit();
    metrics_->set_queue_length(queue_.size());

    if (queue_.size() >= config_.scale_up_threshold &&
        workers_.size() < config_.max_threads) {
        add_worker();
    }

    return future;
}

bool ThreadPool::cancel(const std::string& task_id) {
    std::lock_guard lock(mutex_);
    queue_.cancel(task_id);

    auto it = cancel_handlers_.find(task_id);
    if (it != cancel_handlers_.end()) {
        it->second();
        cancel_handlers_.erase(it);
        void_promises_.erase(task_id);
        completion_callbacks_.erase(task_id);
        return true;
    }

    return false;
}

void ThreadPool::set_completion_handler(const std::string& task_id,
                                        std::function<void(TaskStatus)> handler) {
    std::lock_guard lock(mutex_);
    completion_callbacks_[task_id] = std::move(handler);
}

void ThreadPool::pause() {
    std::lock_guard lock(mutex_);
    paused_ = true;
}

void ThreadPool::resume() {
    std::lock_guard lock(mutex_);
    paused_ = false;
    cv_.notify_all();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
        paused_ = false;
        completion_callbacks_.clear();
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        worker.reset();
    }
    workers_.clear();
    metrics_->set_active_threads(0);
}

ThreadPoolStats ThreadPool::stats() const {
    std::lock_guard lock(mutex_);
    ThreadPoolStats s;
    s.active_threads = workers_.size();
    s.pending_tasks = queue_.size();
    s.completed_tasks = total_completed_;
    s.failed_tasks = total_failed_;
    s.total_submitted = total_completed_ + total_failed_ + queue_.size();
    s.total_cancelled = 0;

    auto snap = metrics_->snapshot();
    s.running_tasks = snap.running_tasks;
    s.peak_queue_length = snap.peak_queue_length;
    s.avg_wait_time_ms = snap.avg_wait_time_ms;
    s.avg_exec_time_ms = snap.avg_exec_time_ms;
    s.worker_utilization = snap.worker_utilization;
    s.tasks_per_sec = snap.tasks_per_sec;
    auto wall = std::chrono::steady_clock::now() - snap.timestamp;
    s.uptime_seconds = std::chrono::duration<double>(wall).count();
    return s;
}

bool ThreadPool::is_running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

bool ThreadPool::is_paused() const noexcept {
    std::lock_guard lock(mutex_);
    return paused_;
}

std::size_t ThreadPool::thread_count() const noexcept {
    std::lock_guard lock(mutex_);
    return workers_.size();
}

std::shared_ptr<Metrics> ThreadPool::metrics() const noexcept {
    return metrics_;
}

std::vector<WorkerInfo> ThreadPool::get_worker_infos() const {
    std::lock_guard lock(mutex_);
    std::vector<WorkerInfo> result;
    result.reserve(workers_.size());
    for (const auto& w : workers_) {
        if (w) {
            WorkerInfo info;
            info.id = w->id();
            info.state = detail::Worker::state_to_string(w->state());
            info.marked_stop = w->is_marked_stop();
            result.push_back(std::move(info));
        }
    }
    return result;
}

QueueInfo ThreadPool::queue_info() const {
    std::lock_guard lock(mutex_);
    QueueInfo info;
    info.size = queue_.size();
    info.capacity = config_.max_queue_size;
    info.is_full = queue_.is_full();
    return info;
}

void ThreadPool::on_task_complete(const std::string& task_id, TaskStatus status,
                                  std::chrono::milliseconds wait_time,
                                  std::chrono::milliseconds exec_time) {
    std::lock_guard lock(mutex_);

    cancel_handlers_.erase(task_id);

    // Determine if this task should be retried.
    bool will_retry = false;
    if (status == TaskStatus::Failed || status == TaskStatus::TimedOut) {
        auto rit = retry_info_.find(task_id);
        if (rit != retry_info_.end()) {
            auto& rc = retry_counts_[task_id];
            if (rc < rit->second.max_retries) {
                will_retry = true;
            }
        }
    }

    // Mark task as completed for dependency resolution in TaskQueue.
    queue_.mark_completed(task_id);

    // Resolve or defer the void promise.
    auto pit = void_promises_.find(task_id);
    if (pit != void_promises_.end()) {
        if (will_retry) {
            // Defer — promise will be resolved on final completion.
        } else if (status == TaskStatus::Completed) {
            pit->second->set_value();
            void_promises_.erase(pit);
        } else if (status == TaskStatus::Cancelled) {
            try {
                throw std::runtime_error("Task was cancelled");
            } catch (...) {
                pit->second->set_exception(std::current_exception());
            }
            void_promises_.erase(pit);
        } else {
            // Failed or TimedOut with no retries left
            try {
                if (status == TaskStatus::TimedOut) {
                    throw std::runtime_error("Task execution timed out");
                }
                throw std::runtime_error("Task execution failed");
            } catch (...) {
                pit->second->set_exception(std::current_exception());
            }
            void_promises_.erase(pit);
        }
    }

    if (status == TaskStatus::Completed) {
        total_completed_++;
    } else {
        total_failed_++;
    }

    metrics_->record_complete(status, exec_time, wait_time);
    metrics_->set_queue_length(queue_.size());
    metrics_->tick();

    history_->record_complete(task_id, status, exec_time);

    auto cb = completion_callbacks_.find(task_id);
    if (cb != completion_callbacks_.end()) {
        if (!will_retry) {
            cb->second(status);
        }
        completion_callbacks_.erase(cb);
    }

    // ── RETRY: re-enqueue if retries remain ────────────────────────────
    // Retry logic lives here in the pool, not in Worker.  Workers stay
    // simple — they execute once and report result.
    if (will_retry) {
        auto rit = retry_info_.find(task_id);
        auto& rc = retry_counts_[task_id];
        ++rc;
        RetryInfo& info = rit->second;

        Task retry_task(info.func, info.name, info.priority);
        retry_task.set_id(task_id);
        retry_task.set_max_retries(info.max_retries);
        retry_task.set_retry_delay(info.retry_delay);
        retry_task.set_timeout(info.timeout_ms);
        retry_task.set_depends_on(info.depends_on);
        retry_task.set_status(TaskStatus::Queued);
        queue_.push(std::move(retry_task));
        cv_.notify_one();

        metrics_->set_queue_length(queue_.size());
    } else {
        // All retries exhausted or task succeeded — clean up retry state
        retry_info_.erase(task_id);
        retry_counts_.erase(task_id);
    }

    try_scale_down();
}

void ThreadPool::add_worker() {
    auto id = next_worker_id_.fetch_add(1, std::memory_order_relaxed);
    auto worker = std::make_unique<detail::Worker>(
        id, mutex_, cv_, queue_, running_, paused_,
        [this](const std::string& task_id, TaskStatus status,
               std::chrono::milliseconds wait_time,
               std::chrono::milliseconds exec_time) {
            on_task_complete(task_id, status, wait_time, exec_time);
        },
        [this]() { metrics_->record_start(); });
    workers_.push_back(std::move(worker));
    metrics_->set_active_threads(workers_.size());
}

void ThreadPool::try_scale_down() {
    std::size_t live = workers_.size() - stopping_count_;
    if (queue_.empty() && live > config_.min_threads) {
        std::size_t excess = live - config_.min_threads;
        for (auto& w : workers_) {
            if (excess == 0) break;
            if (w && w->state() == detail::Worker::State::Waiting && !w->is_marked_stop()) {
                w->request_stop();
                ++stopping_count_;
                --excess;
            }
        }
    }
}

void ThreadPool::reap_stopped_workers() {
    auto it = workers_.begin();
    while (it != workers_.end()) {
        if ((*it)->is_stopped()) {
            it->reset();
            it = workers_.erase(it);
            if (stopping_count_ > 0) --stopping_count_;
        } else {
            ++it;
        }
    }
    metrics_->set_active_threads(workers_.size());
}

std::shared_ptr<TaskHistory> ThreadPool::task_history() const noexcept {
    return history_;
}

std::vector<std::string> ThreadPool::submit_batch(std::vector<Task> tasks) {
    std::vector<std::string> ids;
    ids.reserve(tasks.size());

    std::lock_guard lock(mutex_);

    reap_stopped_workers();

    if (!running_) {
        return ids;
    }

    for (auto& task : tasks) {
        if (queue_.is_full()) {
            break;
        }

        std::string id = task.id();
        ids.push_back(id);

        // Store retry metadata
        if (task.max_retries() > 0) {
            RetryInfo info;
            info.func = task.func();
            info.name = task.name();
            info.priority = task.priority();
            info.max_retries = task.max_retries();
            info.retry_delay = task.retry_delay();
            info.timeout_ms = task.timeout_ms();
            info.depends_on = task.depends_on();
            retry_info_[id] = std::move(info);
        }

        history_->record_submit(id, task.name(), task.created_at());

        task.set_status(TaskStatus::Queued);
        queue_.push(std::move(task));
    }

    if (!ids.empty()) {
        cv_.notify_one();
    }

    metrics_->set_queue_length(queue_.size());

    if (queue_.size() >= config_.scale_up_threshold &&
        workers_.size() < config_.max_threads) {
        add_worker();
    }

    return ids;
}

}  // namespace ctee
