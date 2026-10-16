#include "ctee/core/metrics.h"

using namespace std;
namespace ch = std::chrono;

namespace ctee {

Metrics::Metrics(size_t history_size)
    : history_size_(history_size),
      start_time_(ch::steady_clock::now()),
      last_tick_time_(start_time_ - ch::seconds(2)) {}

void Metrics::record_submit(std::size_t count) {
    submitted_.fetch_add(count, std::memory_order_relaxed);
}

void Metrics::record_start() {
    running_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_complete(TaskStatus status,
                              ch::milliseconds exec_time,
                              ch::milliseconds wait_time) {
    running_.fetch_sub(1, memory_order_relaxed);

    if (status == TaskStatus::Completed) {
        completed_.fetch_add(1, memory_order_relaxed);
    } else if (status == TaskStatus::Failed) {
        failed_.fetch_add(1, memory_order_relaxed);
    }

    {
        lock_guard lock(timing_mutex_);
        total_exec_time_ += exec_time;
        total_wait_time_ += wait_time;
        ++exec_samples_;
        ++wait_samples_;
    }

    total_busy_ns_.fetch_add(
        ch::duration_cast<ch::nanoseconds>(exec_time).count(),
        memory_order_relaxed);
}

void Metrics::record_cancel() {
    cancelled_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::set_queue_length(std::size_t len) {
    queue_length_.store(len, std::memory_order_relaxed);

    std::size_t prev = peak_queue_length_.load(std::memory_order_relaxed);
    while (len > prev) {
        if (peak_queue_length_.compare_exchange_weak(
                prev, len, std::memory_order_relaxed)) {
            break;
        }
    }
}

void Metrics::set_active_threads(std::size_t count) {
    active_threads_.store(count, std::memory_order_relaxed);
}

MetricsSnapshot Metrics::snapshot() const {
    MetricsSnapshot snap;
    snap.submitted_tasks = submitted_.load(memory_order_relaxed);
    snap.completed_tasks = completed_.load(memory_order_relaxed);
    snap.failed_tasks = failed_.load(memory_order_relaxed);
    snap.cancelled_tasks = cancelled_.load(memory_order_relaxed);
    snap.running_tasks = running_.load(memory_order_relaxed);
    snap.queue_length = queue_length_.load(memory_order_relaxed);
    snap.peak_queue_length = peak_queue_length_.load(memory_order_relaxed);
    snap.active_threads = active_threads_.load(memory_order_relaxed);
    snap.timestamp = ch::steady_clock::now();

    {
        lock_guard lock(timing_mutex_);
        if (wait_samples_ > 0) {
            snap.avg_wait_time_ms =
                ch::duration<double, milli>(total_wait_time_).count()
                / static_cast<double>(wait_samples_);
        }
        if (exec_samples_ > 0) {
            snap.avg_exec_time_ms =
                ch::duration<double, milli>(total_exec_time_).count()
                / static_cast<double>(exec_samples_);
        }
    }

    auto wall_time = ch::steady_clock::now() - start_time_;
    auto wall_ns = ch::duration_cast<ch::nanoseconds>(wall_time);
    if (wall_ns.count() > 0 && snap.active_threads > 0) {
        snap.worker_utilization = static_cast<double>(
            total_busy_ns_.load(memory_order_relaxed))
            / static_cast<double>(wall_ns.count() * snap.active_threads);
        if (snap.worker_utilization > 1.0) snap.worker_utilization = 1.0;
    }

    return snap;
}

vector<MetricsSnapshot> Metrics::history() const {
    lock_guard lock(history_mutex_);
    return {history_.begin(), history_.end()};
}

void Metrics::tick() {
    auto now = ch::steady_clock::now();
    auto elapsed = ch::duration_cast<ch::milliseconds>(
        now - last_tick_time_);

    if (elapsed.count() < 1000) return;

    last_tick_time_ = now;
    auto snap = snapshot();

    {
        lock_guard lock(history_mutex_);
        if (!history_.empty()) {
            const auto& prev = history_.back();
            auto dt = ch::duration_cast<ch::milliseconds>(
                snap.timestamp - prev.timestamp);
            if (dt.count() > 0) {
                snap.tasks_per_sec = static_cast<double>(
                    snap.completed_tasks - prev.completed_tasks)
                    / (static_cast<double>(dt.count()) / 1000.0);
            }
        }
    }

    record_snapshot(snap);
}

void Metrics::reset() {
    submitted_.store(0, memory_order_relaxed);
    completed_.store(0, memory_order_relaxed);
    failed_.store(0, memory_order_relaxed);
    cancelled_.store(0, memory_order_relaxed);
    running_.store(0, memory_order_relaxed);
    queue_length_.store(0, memory_order_relaxed);
    peak_queue_length_.store(0, memory_order_relaxed);
    active_threads_.store(0, memory_order_relaxed);
    total_busy_ns_.store(0, memory_order_relaxed);

    {
        lock_guard lock(timing_mutex_);
        total_wait_time_ = {};
        total_exec_time_ = {};
        wait_samples_ = 0;
        exec_samples_ = 0;
    }

    {
        lock_guard lock(history_mutex_);
        history_.clear();
    }

    start_time_ = ch::steady_clock::now();
    last_tick_time_ = start_time_ - ch::seconds(2);
}

void Metrics::record_snapshot(const MetricsSnapshot& snap) {
    lock_guard lock(history_mutex_);
    if (history_.size() >= history_size_) {
        history_.pop_front();
    }
    history_.push_back(snap);
}

}  // namespace ctee
