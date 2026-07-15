#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "ctee/core/task_result.h"

namespace ctee {

// ---------------------------------------------------------------------------
// MetricsSnapshot — a point-in-time copy of all metrics.
//
// Returned by Metrics::snapshot() and embedded in the time-series history.
// The frontend can render these directly in Recharts.
// ---------------------------------------------------------------------------
struct MetricsSnapshot {
    std::size_t submitted_tasks = 0;
    std::size_t completed_tasks = 0;
    std::size_t failed_tasks = 0;
    std::size_t cancelled_tasks = 0;
    std::size_t running_tasks = 0;

    std::size_t queue_length = 0;
    std::size_t peak_queue_length = 0;
    std::size_t active_threads = 0;

    double avg_wait_time_ms = 0.0;
    double avg_exec_time_ms = 0.0;
    double worker_utilization = 0.0;
    double tasks_per_sec = 0.0;

    std::chrono::steady_clock::time_point timestamp;
};

// ---------------------------------------------------------------------------
// Metrics — thread-safe collection of pool metrics with time-series history.
//
// Hot-path recording (submit/complete) uses atomics so it never contends
// with the pool mutex.  Cold-path reads (snapshot/history) take a mutex
// only to copy the history deque and compute derived values.
//
// Thread safety:
//   - All record_* methods are safe to call concurrently from any thread.
//   - snapshot() and history() are safe to call from any thread.
//   - tick() should be called at most once per second (it is idempotent
//     if called more often — it only records when ≥1 s has elapsed).
// ---------------------------------------------------------------------------
class Metrics {
public:
    explicit Metrics(std::size_t history_size = 60);
    ~Metrics() = default;

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    Metrics(Metrics&&) = delete;
    Metrics& operator=(Metrics&&) = delete;

    // ── Hot-path recording (lock-free atomics) ────────────────────────

    void record_submit(std::size_t count = 1);
    void record_start();
    void record_complete(TaskStatus status,
                         std::chrono::milliseconds exec_time,
                         std::chrono::milliseconds wait_time);
    void record_cancel();

    void set_queue_length(std::size_t len);
    void set_active_threads(std::size_t count);

    // ── Cold-path reads ───────────────────────────────────────────────

    [[nodiscard]] MetricsSnapshot snapshot() const;
    [[nodiscard]] std::vector<MetricsSnapshot> history() const;

    // ── Time-series housekeeping ──────────────────────────────────────

    void tick();

    // ── Reset ─────────────────────────────────────────────────────────

    void reset();

private:
    void record_snapshot(const MetricsSnapshot& snap);

    std::size_t history_size_;

    // Counters (atomics for lock-free recording)
    std::atomic<std::size_t> submitted_{0};
    std::atomic<std::size_t> completed_{0};
    std::atomic<std::size_t> failed_{0};
    std::atomic<std::size_t> cancelled_{0};
    std::atomic<std::size_t> running_{0};

    // State (atomics)
    std::atomic<std::size_t> queue_length_{0};
    std::atomic<std::size_t> peak_queue_length_{0};
    std::atomic<std::size_t> active_threads_{0};

    // Timing accumulators (mutex-protected for compound reads)
    mutable std::mutex timing_mutex_;
    std::chrono::nanoseconds total_wait_time_{0};
    std::chrono::nanoseconds total_exec_time_{0};
    std::size_t wait_samples_{0};
    std::size_t exec_samples_{0};

    // Worker utilization tracking
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<long long> total_busy_ns_{0};

    // Time-series history
    mutable std::mutex history_mutex_;
    std::deque<MetricsSnapshot> history_;
    std::chrono::steady_clock::time_point last_tick_time_;
};

}  // namespace ctee
