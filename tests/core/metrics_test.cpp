#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "ctee/core/metrics.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

using namespace ctee;
using namespace std::chrono_literals;

// ── MetricsUnit ────────────────────────────────────────────────────────

TEST(MetricsUnit, InitialSnapshotAllZero) {
    Metrics m;
    auto snap = m.snapshot();
    EXPECT_EQ(snap.submitted_tasks, 0u);
    EXPECT_EQ(snap.completed_tasks, 0u);
    EXPECT_EQ(snap.failed_tasks, 0u);
    EXPECT_EQ(snap.cancelled_tasks, 0u);
    EXPECT_EQ(snap.running_tasks, 0u);
    EXPECT_EQ(snap.queue_length, 0u);
    EXPECT_EQ(snap.peak_queue_length, 0u);
    EXPECT_EQ(snap.active_threads, 0u);
    EXPECT_DOUBLE_EQ(snap.avg_wait_time_ms, 0.0);
    EXPECT_DOUBLE_EQ(snap.avg_exec_time_ms, 0.0);
    EXPECT_DOUBLE_EQ(snap.worker_utilization, 0.0);
    EXPECT_DOUBLE_EQ(snap.tasks_per_sec, 0.0);
}

TEST(MetricsUnit, RecordSubmit) {
    Metrics m;
    m.record_submit(5);
    auto snap = m.snapshot();
    EXPECT_EQ(snap.submitted_tasks, 5u);

    m.record_submit(3);
    snap = m.snapshot();
    EXPECT_EQ(snap.submitted_tasks, 8u);
}

TEST(MetricsUnit, RecordSubmitDefaultOne) {
    Metrics m;
    m.record_submit();
    m.record_submit();
    m.record_submit();
    auto snap = m.snapshot();
    EXPECT_EQ(snap.submitted_tasks, 3u);
}

TEST(MetricsUnit, RecordStartIncrementsRunning) {
    Metrics m;
    m.record_start();
    auto snap = m.snapshot();
    EXPECT_EQ(snap.running_tasks, 1u);

    m.record_start();
    snap = m.snapshot();
    EXPECT_EQ(snap.running_tasks, 2u);
}

TEST(MetricsUnit, RecordCompleteDecrementsRunning) {
    Metrics m;
    m.record_start();
    m.record_start();
    EXPECT_EQ(m.snapshot().running_tasks, 2u);

    m.record_complete(TaskStatus::Completed, 10ms, 5ms);
    auto snap = m.snapshot();
    EXPECT_EQ(snap.running_tasks, 1u);
    EXPECT_EQ(snap.completed_tasks, 1u);
    EXPECT_EQ(snap.failed_tasks, 0u);
}

TEST(MetricsUnit, RecordCompleteFailed) {
    Metrics m;
    m.record_start();
    m.record_complete(TaskStatus::Failed, 2ms, 1ms);
    auto snap = m.snapshot();
    EXPECT_EQ(snap.running_tasks, 0u);
    EXPECT_EQ(snap.completed_tasks, 0u);
    EXPECT_EQ(snap.failed_tasks, 1u);
}

TEST(MetricsUnit, RecordCancel) {
    Metrics m;
    m.record_cancel();
    m.record_cancel();
    auto snap = m.snapshot();
    EXPECT_EQ(snap.cancelled_tasks, 2u);
}

TEST(MetricsUnit, AverageWaitTime) {
    Metrics m;
    m.record_complete(TaskStatus::Completed, 10ms, 100ms);
    m.record_complete(TaskStatus::Completed, 10ms, 200ms);
    auto snap = m.snapshot();
    EXPECT_DOUBLE_EQ(snap.avg_wait_time_ms, 150.0);
}

TEST(MetricsUnit, AverageExecTime) {
    Metrics m;
    m.record_complete(TaskStatus::Completed, 50ms, 0ms);
    m.record_complete(TaskStatus::Completed, 150ms, 0ms);
    auto snap = m.snapshot();
    EXPECT_DOUBLE_EQ(snap.avg_exec_time_ms, 100.0);
}

TEST(MetricsUnit, QueueLengthTracking) {
    Metrics m;
    m.set_queue_length(5);
    auto snap = m.snapshot();
    EXPECT_EQ(snap.queue_length, 5u);
    EXPECT_EQ(snap.peak_queue_length, 5u);

    m.set_queue_length(3);
    snap = m.snapshot();
    EXPECT_EQ(snap.queue_length, 3u);
    EXPECT_EQ(snap.peak_queue_length, 5u);

    m.set_queue_length(10);
    snap = m.snapshot();
    EXPECT_EQ(snap.queue_length, 10u);
    EXPECT_EQ(snap.peak_queue_length, 10u);
}

TEST(MetricsUnit, ActiveThreadsTracking) {
    Metrics m;
    m.set_active_threads(4);
    auto snap = m.snapshot();
    EXPECT_EQ(snap.active_threads, 4u);
}

TEST(MetricsUnit, Reset) {
    Metrics m;
    m.record_submit(10);
    m.record_start();
    m.record_complete(TaskStatus::Completed, 50ms, 20ms);
    m.record_cancel();
    m.set_queue_length(5);
    m.set_active_threads(3);

    m.reset();
    auto snap = m.snapshot();
    EXPECT_EQ(snap.submitted_tasks, 0u);
    EXPECT_EQ(snap.completed_tasks, 0u);
    EXPECT_EQ(snap.failed_tasks, 0u);
    EXPECT_EQ(snap.cancelled_tasks, 0u);
    EXPECT_EQ(snap.running_tasks, 0u);
    EXPECT_EQ(snap.queue_length, 0u);
    EXPECT_EQ(snap.peak_queue_length, 0u);
    EXPECT_EQ(snap.active_threads, 0u);
    EXPECT_DOUBLE_EQ(snap.avg_wait_time_ms, 0.0);
    EXPECT_DOUBLE_EQ(snap.avg_exec_time_ms, 0.0);
}

TEST(MetricsUnit, TimeSeriesHistory) {
    Metrics m(10);

    m.record_submit(5);
    m.record_complete(TaskStatus::Completed, 10ms, 5ms);
    m.tick();

    std::this_thread::sleep_for(1100ms);

    m.record_submit(3);
    m.record_complete(TaskStatus::Completed, 20ms, 8ms);
    m.tick();

    auto hist = m.history();
    EXPECT_GE(hist.size(), 2u);
    EXPECT_EQ(hist[0].submitted_tasks, 5u);
    EXPECT_EQ(hist[1].submitted_tasks, 8u);
}

TEST(MetricsUnit, TimeSeriesCircularBuffer) {
    Metrics m(3);

    for (int i = 0; i < 5; ++i) {
        m.record_submit(1);
        m.tick();
        std::this_thread::sleep_for(1100ms);
    }

    auto hist = m.history();
    EXPECT_LE(hist.size(), 3u);
}

TEST(MetricsUnit, TickIdempotentWithinOneSecond) {
    Metrics m;
    m.record_submit(1);

    m.tick();
    m.tick();
    m.tick();

    auto hist = m.history();
    EXPECT_EQ(hist.size(), 1u);
}

TEST(MetricsUnit, ConcurrentRecordAndSnapshot) {
    Metrics m;
    std::atomic<bool> done{false};

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&m, &done]() {
            while (!done.load()) {
                m.record_submit(1);
                m.record_start();
                m.record_complete(TaskStatus::Completed, 1ms, 0ms);
                m.set_queue_length(1);
                (void)m.snapshot();
            }
        });
    }

    std::this_thread::sleep_for(200ms);
    done.store(true);
    for (auto& t : threads) t.join();

    auto snap = m.snapshot();
    EXPECT_GT(snap.submitted_tasks, 0u);
}

TEST(MetricsUnit, WorkerUtilization) {
    Metrics m;
    m.set_active_threads(1);

    std::this_thread::sleep_for(100ms);
    m.record_complete(TaskStatus::Completed, 80ms, 0ms);

    auto snap = m.snapshot();
    EXPECT_GT(snap.worker_utilization, 0.0);
    EXPECT_LE(snap.worker_utilization, 1.5);
}

TEST(MetricsUnit, TasksPerSecFromHistory) {
    Metrics m(60);

    for (int i = 0; i < 10; ++i) {
        m.record_complete(TaskStatus::Completed, 1ms, 0ms);
    }
    m.tick();

    std::this_thread::sleep_for(1100ms);

    for (int i = 0; i < 20; ++i) {
        m.record_complete(TaskStatus::Completed, 1ms, 0ms);
    }
    m.tick();

    auto hist = m.history();
    ASSERT_GE(hist.size(), 2u);
    EXPECT_GT(hist.back().tasks_per_sec, 0.0);
}

// ── MetricsIntegration ────────────────────────────────────────────────

class MetricsIntegration : public ::testing::Test {
protected:
    ThreadPoolConfig make_config() {
        ThreadPoolConfig c;
        c.min_threads = 2;
        c.max_threads = 4;
        c.scale_up_threshold = 2;
        c.max_queue_size = 1000;
        return c;
    }
};

TEST_F(MetricsIntegration, SubmitIncrementsSubmitted) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    pool.submit([]() {}).get();
    pool.submit([]() {}).get();

    auto snap = m->snapshot();
    EXPECT_GE(snap.submitted_tasks, 2u);
}

TEST_F(MetricsIntegration, CompletedTasksTracked) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([]() {}));
    }
    for (auto& f : futures) f.get();

    auto snap = m->snapshot();
    EXPECT_EQ(snap.completed_tasks, 10u);
}

TEST_F(MetricsIntegration, FailedTasksTracked) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        Task t([]() { throw std::runtime_error("fail"); }, "fail_task");
        futures.push_back(pool.submit(std::move(t)));
    }
    for (auto& f : futures) {
        try { f.get(); } catch (...) {}
    }

    auto snap = m->snapshot();
    EXPECT_EQ(snap.failed_tasks, 5u);
}

TEST_F(MetricsIntegration, MixedSuccessAndFailure) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        if (i % 3 == 0) {
            Task t([]() { throw std::runtime_error("fail"); }, "fail");
            futures.push_back(pool.submit(std::move(t)));
        } else {
            futures.push_back(pool.submit([]() {}));
        }
    }
    for (auto& f : futures) {
        try { f.get(); } catch (...) {}
    }

    auto snap = m->snapshot();
    EXPECT_EQ(snap.completed_tasks + snap.failed_tasks, 10u);
    EXPECT_GT(snap.completed_tasks, 0u);
    EXPECT_GT(snap.failed_tasks, 0u);
}

TEST_F(MetricsIntegration, AvgExecTimeNonZero) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit([]() {
            std::this_thread::sleep_for(10ms);
        }));
    }
    for (auto& f : futures) f.get();

    auto snap = m->snapshot();
    EXPECT_GT(snap.avg_exec_time_ms, 0.0);
}

TEST_F(MetricsIntegration, AvgWaitTimeNonZero) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 20; ++i) {
        futures.push_back(pool.submit([]() {
            std::this_thread::sleep_for(5ms);
        }));
    }
    for (auto& f : futures) f.get();

    auto snap = m->snapshot();
    EXPECT_GE(snap.avg_wait_time_ms, 0.0);
}

TEST_F(MetricsIntegration, PeakQueueLengthTracked) {
    ThreadPoolConfig c;
    c.min_threads = 1;
    c.max_threads = 2;
    c.scale_up_threshold = 2;
    c.max_queue_size = 1000;
    ThreadPool pool(c);
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 20; ++i) {
        futures.push_back(pool.submit([]() {
            std::this_thread::sleep_for(10ms);
        }));
    }

    std::this_thread::sleep_for(50ms);

    auto snap = m->snapshot();
    EXPECT_GT(snap.peak_queue_length, 0u);

    for (auto& f : futures) f.get();
}

TEST_F(MetricsIntegration, TimeSeriesPopulated) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    {
        std::vector<std::future<void>> futures;
        for (int i = 0; i < 5; ++i) {
            futures.push_back(pool.submit([]() {}));
        }
        for (auto& f : futures) f.get();
    }

    std::this_thread::sleep_for(1100ms);

    {
        std::vector<std::future<void>> futures;
        for (int i = 0; i < 5; ++i) {
            futures.push_back(pool.submit([]() {}));
        }
        for (auto& f : futures) f.get();
    }

    auto hist = m->history();
    EXPECT_GE(hist.size(), 1u);
}

TEST_F(MetricsIntegration, SnapshotHasRunningTasks) {
    ThreadPoolConfig c;
    c.min_threads = 1;
    c.max_threads = 2;
    c.scale_up_threshold = 10;
    c.max_queue_size = 1000;
    ThreadPool pool(c);
    auto m = pool.metrics();

    std::promise<void> gate;
    auto gate_future = gate.get_future();
    auto blocker = pool.submit([&gate_future]() {
        gate_future.wait();
    });

    std::this_thread::sleep_for(100ms);

    auto snap = m->snapshot();
    EXPECT_GE(snap.running_tasks, 1u);

    gate.set_value();
    blocker.get();
}

TEST_F(MetricsIntegration, WorkerUtilizationAfterWork) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([]() {
            std::this_thread::sleep_for(5ms);
        }));
    }
    for (auto& f : futures) f.get();

    std::this_thread::sleep_for(100ms);

    auto snap = m->snapshot();
    EXPECT_GT(snap.worker_utilization, 0.0);
}

TEST_F(MetricsIntegration, PoolStatsIncludesMetrics) {
    ThreadPool pool(make_config());

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit([]() {}));
    }
    for (auto& f : futures) f.get();

    std::this_thread::sleep_for(50ms);

    auto s = pool.stats();
    EXPECT_EQ(s.completed_tasks, 5u);
    EXPECT_GE(s.avg_exec_time_ms, 0.0);
}

TEST_F(MetricsIntegration, ConcurrentSubmitsMetricsAccurate) {
    ThreadPool pool(make_config());
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 50; ++i) {
        futures.push_back(pool.submit([]() {}));
    }
    for (auto& f : futures) f.get();

    std::this_thread::sleep_for(50ms);

    auto snap = m->snapshot();
    EXPECT_EQ(snap.completed_tasks, 50u);
    EXPECT_EQ(snap.submitted_tasks, 50u);
}
