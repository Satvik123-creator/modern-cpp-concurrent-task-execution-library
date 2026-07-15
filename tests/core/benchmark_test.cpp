#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "ctee/core/metrics.h"
#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

using detail::TaskQueue;

ThreadPoolConfig bench_config(std::size_t threads = 8) {
    ThreadPoolConfig c;
    c.min_threads = threads;
    c.max_threads = threads;
    c.max_queue_size = 50000;
    c.enable_metrics = true;
    return c;
}

// ---------------------------------------------------------------------------
// 1. BenchmarkSubmitThroughput
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkSubmitThroughput) {
    ThreadPool pool(bench_config(8));

    constexpr std::size_t kTaskCount = 10000;
    std::atomic<std::size_t> completed{0};

    auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        pool.submit([&completed]() { completed.fetch_add(1, std::memory_order_relaxed); });
    }

    // Wait for all tasks to finish.
    while (completed.load(std::memory_order_relaxed) < kTaskCount) {
        std::this_thread::yield();
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    pool.shutdown();

    double tasks_per_sec = (elapsed_ms > 0)
        ? static_cast<double>(kTaskCount) * 1000.0 / elapsed_ms
        : static_cast<double>(kTaskCount) * 1000.0;

    EXPECT_GT(tasks_per_sec, 5000.0)
        << "Submit throughput " << tasks_per_sec << " tasks/sec is below 5000 threshold";
}

// ---------------------------------------------------------------------------
// 2. BenchmarkNoopTaskLatency
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkNoopTaskLatency) {
    ThreadPool pool(bench_config(1));

    constexpr std::size_t kTaskCount = 1000;

    auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        auto f = pool.submit([]() {});
        f.get();
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    pool.shutdown();

    double avg_us = static_cast<double>(elapsed_us) / kTaskCount;
    double avg_ms = avg_us / 1000.0;

    EXPECT_LT(avg_ms, 1.0)
        << "Average noop task latency " << avg_ms << " ms exceeds 1 ms threshold";
}

// ---------------------------------------------------------------------------
// 3. BenchmarkQueuePushPopThroughput
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkQueuePushPopThroughput) {
    constexpr std::size_t kOps = 100000;
    TaskQueue queue(kOps);

    // Pre-build tasks so construction cost is excluded from timing.
    std::vector<Task> tasks;
    tasks.reserve(kOps);
    for (std::size_t i = 0; i < kOps; ++i) {
        tasks.push_back(Task([]() {}, "bench_task"));
    }

    auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kOps; ++i) {
        queue.push(std::move(tasks[i]));
    }

    std::size_t pop_count = 0;
    while (auto t = queue.pop()) {
        ++pop_count;
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_EQ(pop_count, kOps);

    double ops_per_sec = (elapsed_ms > 0)
        ? static_cast<double>(kOps) * 1000.0 / elapsed_ms
        : static_cast<double>(kOps) * 1000.0;

    EXPECT_GT(ops_per_sec, 100000.0)
        << "Queue push/pop throughput " << ops_per_sec << " ops/sec is below 100000 threshold";
}

// ---------------------------------------------------------------------------
// 4. BenchmarkBatchThroughput
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkBatchThroughput) {
    // Batch submission reduces API call overhead: 10 calls vs 10000 calls.
    // We verify that batch submit is functional and completes within a
    // reasonable time, not that it's faster than individual (contention
    // characteristics differ).
    ThreadPool pool(bench_config(8));

    constexpr std::size_t kTotal = 10000;
    std::atomic<std::size_t> completed{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<Task> batch;
    batch.reserve(kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
        batch.push_back(Task([&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    pool.submit_batch(std::move(batch));

    while (completed.load(std::memory_order_relaxed) < kTotal) {
        std::this_thread::yield();
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    pool.shutdown();

    EXPECT_EQ(completed.load(), kTotal);
    EXPECT_LT(elapsed_ms, 10000)
        << "Batch of " << kTotal << " tasks took " << elapsed_ms
        << "ms, exceeds 10s threshold";
}

// ---------------------------------------------------------------------------
// 5. BenchmarkPriorityOrderingOverhead
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkPriorityOrderingOverhead) {
    constexpr std::size_t kCount = 10000;
    TaskQueue queue(kCount);

    std::mt19937 rng(42);
    std::vector<Task> tasks;
    tasks.reserve(kCount);

    TaskPriority levels[] = {
        TaskPriority::Low, TaskPriority::Normal,
        TaskPriority::High, TaskPriority::Critical
    };

    for (std::size_t i = 0; i < kCount; ++i) {
        auto prio = levels[rng() % 4];
        tasks.push_back(Task([]() {}, "prio_task", prio));
    }

    auto start = std::chrono::steady_clock::now();

    for (auto& t : tasks) {
        queue.push(std::move(t));
    }

    std::size_t pop_count = 0;
    while (auto t = queue.pop()) {
        ++pop_count;
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_EQ(pop_count, kCount);
    EXPECT_LT(elapsed_ms, 200)
        << "Priority ordering overhead " << elapsed_ms
        << "ms for " << kCount << " tasks exceeds 200ms threshold";
}

// ---------------------------------------------------------------------------
// 6. BenchmarkMetricsSnapshotLatency
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkMetricsSnapshotLatency) {
    Metrics metrics;

    constexpr std::size_t kCount = 10000;

    // Seed some data so the snapshot does real work.
    for (std::size_t i = 0; i < 100; ++i) {
        metrics.record_submit();
        metrics.record_start();
        metrics.record_complete(TaskStatus::Completed, std::chrono::milliseconds(1),
                                std::chrono::milliseconds(0));
    }
    metrics.tick();

    auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kCount; ++i) {
        volatile auto snap = metrics.snapshot();
        (void)snap;
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double avg_us = static_cast<double>(elapsed_us) / kCount;

    EXPECT_LT(avg_us, 10.0)
        << "Average metrics snapshot latency " << avg_us
        << " microseconds exceeds 10us threshold";
}

// ---------------------------------------------------------------------------
// 7. BenchmarkTaskCreationOverhead
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkTaskCreationOverhead) {
    constexpr std::size_t kCount = 10000;

    auto start = std::chrono::steady_clock::now();

    std::vector<Task> tasks;
    tasks.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        tasks.push_back(Task([]() {}, "creation_bench"));
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_LT(elapsed_ms, 100)
        << "Task creation overhead " << elapsed_ms
        << "ms for " << kCount << " tasks exceeds 100ms threshold";
}

// ---------------------------------------------------------------------------
// 8. BenchmarkConcurrentSubmit4Threads
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkConcurrentSubmit4Threads) {
    ThreadPool pool(bench_config(4));

    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 2500;
    constexpr std::size_t kTotal = kThreads * kPerThread;

    std::atomic<std::size_t> completed{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> submitters;
    for (std::size_t t = 0; t < kThreads; ++t) {
        submitters.emplace_back([&pool, &completed]() {
            for (std::size_t i = 0; i < kPerThread; ++i) {
                pool.submit([&completed]() {
                    completed.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& th : submitters) {
        th.join();
    }

    while (completed.load(std::memory_order_relaxed) < kTotal) {
        std::this_thread::yield();
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    pool.shutdown();

    double tasks_per_sec = (elapsed_ms > 0)
        ? static_cast<double>(kTotal) * 1000.0 / elapsed_ms
        : static_cast<double>(kTotal) * 1000.0;

    EXPECT_GT(tasks_per_sec, 10000.0)
        << "Concurrent submit throughput " << tasks_per_sec
        << " tasks/sec is below 10000 threshold";
}

// ---------------------------------------------------------------------------
// 9. BenchmarkScaleUpDown
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkScaleUpDown) {
    ThreadPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 8;
    cfg.scale_up_threshold = 5;
    cfg.idle_timeout = std::chrono::milliseconds(1000);
    cfg.max_queue_size = 10000;
    cfg.enable_metrics = true;

    ThreadPool pool(cfg);

    ASSERT_EQ(pool.thread_count(), cfg.min_threads);

    // Submit enough tasks to trigger scale-up.
    std::atomic<std::size_t> done{0};
    for (std::size_t i = 0; i < 100; ++i) {
        pool.submit([&done]() {
            done.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }

    // Wait for completion.
    while (done.load(std::memory_order_relaxed) < 100) {
        std::this_thread::yield();
    }

    // After tasks drain, scale-down should eventually return to min_threads.
    // Scale-down is gradual (one per on_task_complete when queue is empty).
    // Stopped workers are only reaped in submit(), so we submit dummy tasks
    // to trigger reaping.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool at_min = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (pool.thread_count() <= cfg.min_threads) {
            at_min = true;
            break;
        }
        // Submit dummy task to trigger reap_stopped_workers()
        pool.submit([]() {});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    pool.shutdown();

    EXPECT_TRUE(at_min)
        << "Pool did not scale down to min_threads within 10 seconds";
}

// ---------------------------------------------------------------------------
// 10. BenchmarkHistoryQuery
// ---------------------------------------------------------------------------
TEST(BenchmarkTest, BenchmarkHistoryQuery) {
    TaskHistory history(5000);

    constexpr std::size_t kRecordCount = 1000;
    constexpr std::size_t kQueryCount = 1000;
    constexpr std::size_t kQuerySize = 100;

    for (std::size_t i = 0; i < kRecordCount; ++i) {
        TaskRecord rec;
        rec.task_id = "task_" + std::to_string(i);
        rec.task_name = "bench";
        rec.status = TaskStatus::Completed;
        rec.submitted_at = std::chrono::steady_clock::now();
        rec.completed_at = std::chrono::steady_clock::now();
        rec.execution_time = std::chrono::milliseconds(1);
        history.record(rec);
    }

    auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < kQueryCount; ++i) {
        volatile auto result = history.recent(kQuerySize);
        (void)result;
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_LT(elapsed_ms, 50)
        << "History query overhead " << elapsed_ms
        << "ms for " << kQueryCount << " queries exceeds 50ms threshold";
}

}  // namespace
}  // namespace ctee
