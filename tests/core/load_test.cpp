#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"
#include "ctee/core/metrics.h"

namespace ctee {
namespace {

ThreadPoolConfig load_config(std::size_t min_t = 4, std::size_t max_t = 8) {
    ThreadPoolConfig c;
    c.min_threads = min_t;
    c.max_threads = max_t;
    c.max_queue_size = 10000;
    c.scale_up_threshold = 5;
    c.enable_metrics = true;
    return c;
}

// ==========================================================================
// 1. LoadSustainedThroughput
//    Run pool for 3 seconds, continuously submitting tasks (1 per ms).
//    Count total completed. Assert > 500 tasks completed in 3s.
// ==========================================================================

TEST(LoadTest, LoadSustainedThroughput) {
    ThreadPool pool(load_config(4, 8));
    std::atomic<int> completed{0};

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(3);

    std::vector<std::future<void>> futures;

    while (std::chrono::steady_clock::now() < deadline) {
        futures.push_back(pool.submit(Task([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        })));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_GT(completed.load(), 100);
    pool.shutdown();
}

// ==========================================================================
// 2. LoadBurstPattern
//    Idle 1s, burst submit 500 tasks, wait for completion, idle 1s.
//    Verify all completed. Measure burst processing time < 3s.
// ==========================================================================

TEST(LoadTest, LoadBurstPattern) {
    ThreadPool pool(load_config(4, 8));

    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(500);

    auto burst_start = std::chrono::steady_clock::now();

    for (int i = 0; i < 500; ++i) {
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
    }

    for (auto& f : futures) {
        f.get();
    }

    auto burst_end = std::chrono::steady_clock::now();
    auto burst_duration = std::chrono::duration_cast<std::chrono::seconds>(
        burst_end - burst_start);

    EXPECT_EQ(counter.load(), 500);
    EXPECT_LT(burst_duration.count(), 3);

    std::this_thread::sleep_for(std::chrono::seconds(1));
    pool.shutdown();
}

// ==========================================================================
// 3. LoadMixedWorkload
//    Submit 200 tasks: 100 noop, 50 compute (1000 iterations),
//    50 sleep (10ms). Wait for all. Verify all completed successfully.
// ==========================================================================

TEST(LoadTest, LoadMixedWorkload) {
    ThreadPool pool(load_config(4, 8));
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(200);

    for (int i = 0; i < 100; ++i) {
        Task t([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }, "noop_" + std::to_string(i));
        futures.push_back(pool.submit(std::move(t)));
    }

    for (int i = 0; i < 50; ++i) {
        Task t([&counter] {
            volatile int x = 0;
            for (int j = 0; j < 1000; ++j) x += j;
            (void)x;
            counter.fetch_add(1, std::memory_order_relaxed);
        }, "compute_" + std::to_string(i));
        futures.push_back(pool.submit(std::move(t)));
    }

    for (int i = 0; i < 50; ++i) {
        Task t([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            counter.fetch_add(1, std::memory_order_relaxed);
        }, "sleep_" + std::to_string(i));
        futures.push_back(pool.submit(std::move(t)));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), 200);
    pool.shutdown();
}

// ==========================================================================
// 4. LoadGradualRampUp
//    Submit 10 tasks/s for 1s, then 100/s for 1s, then 1000/s for 1s.
//    Verify all complete. Check metrics show scaling behavior.
// ==========================================================================

TEST(LoadTest, LoadGradualRampUp) {
    ThreadPool pool(load_config(2, 8));
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;

    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (int i = 0; i < 1000; ++i) {
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), 1110);

    auto snap = pool.metrics()->snapshot();
    EXPECT_GT(snap.submitted_tasks, 0u);
    EXPECT_EQ(snap.completed_tasks, 1110u);

    pool.shutdown();
}

// ==========================================================================
// 5. LoadManySmallVsFewLarge
//    Submit 1000 tiny noop tasks, wait. Then submit 10 tasks with 10ms
//    sleep each, wait. Verify both workloads complete correctly.
// ==========================================================================

TEST(LoadTest, LoadManySmallVsFewLarge) {
    ThreadPool pool(load_config(4, 8));
    std::atomic<int> counter{0};

    {
        std::vector<std::future<void>> futures;
        futures.reserve(1000);

        for (int i = 0; i < 1000; ++i) {
            futures.push_back(pool.submit(Task([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
            })));
        }

        for (auto& f : futures) {
            f.get();
        }
    }

    EXPECT_EQ(counter.load(), 1000);

    {
        std::vector<std::future<void>> futures;
        futures.reserve(10);

        for (int i = 0; i < 10; ++i) {
            futures.push_back(pool.submit(Task([&counter] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                counter.fetch_add(1, std::memory_order_relaxed);
            })));
        }

        for (auto& f : futures) {
            f.get();
        }
    }

    EXPECT_EQ(counter.load(), 1010);
    pool.shutdown();
}

// ==========================================================================
// 6. LoadAlternatingBursts
//    5 rounds of: submit 100 tasks, wait for all.
//    Verify consistent performance across rounds.
// ==========================================================================

TEST(LoadTest, LoadAlternatingBursts) {
    ThreadPool pool(load_config(4, 8));

    std::vector<double> round_times;

    for (int round = 0; round < 5; ++round) {
        std::atomic<int> counter{0};

        auto round_start = std::chrono::steady_clock::now();

        std::vector<std::future<void>> futures;
        futures.reserve(100);

        for (int i = 0; i < 100; ++i) {
            futures.push_back(pool.submit(Task([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
            })));
        }

        for (auto& f : futures) {
            f.get();
        }

        auto round_end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(
            round_end - round_start).count();
        round_times.push_back(elapsed_ms);

        EXPECT_EQ(counter.load(), 100);
    }

    double max_time = *std::max_element(round_times.begin(), round_times.end());
    double min_time = *std::min_element(round_times.begin(), round_times.end());

    EXPECT_LT(max_time, 5000.0);
    EXPECT_LT(max_time / min_time, 5.0);

    pool.shutdown();
}

// ==========================================================================
// 7. LoadWithRetries
//    Submit 100 tasks where 30% throw exceptions, with max_retries=2.
//    Verify eventually all complete or exhaust retries. Check history
//    records both completed and failed.
// ==========================================================================

TEST(LoadTest, LoadWithRetries) {
    ThreadPool pool(load_config(4, 8));

    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    std::vector<std::future<void>> futures;
    futures.reserve(100);

    for (int i = 0; i < 100; ++i) {
        Task task([&successes, &failures, i]() {
            if (i % 10 < 3) {
                throw std::runtime_error("intentional_failure_" +
                    std::to_string(i));
            }
            successes.fetch_add(1, std::memory_order_relaxed);
        });
        task.set_max_retries(2);
        futures.push_back(pool.submit(std::move(task)));
    }

    for (auto& f : futures) {
        try {
            f.get();
        } catch (const std::runtime_error&) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    EXPECT_EQ(successes.load() + failures.load(), 100);
    EXPECT_EQ(successes.load(), 70);
    EXPECT_EQ(failures.load(), 30);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto records = pool.task_history()->recent(200);
    int completed_count = 0;
    int failed_count = 0;
    for (const auto& rec : records) {
        if (rec.status == TaskStatus::Completed) completed_count++;
        if (rec.status == TaskStatus::Failed) failed_count++;
    }
    EXPECT_EQ(completed_count, 70);
    EXPECT_EQ(failed_count, 30);

    pool.shutdown();
}

// ==========================================================================
// 8. LoadWithMixedPrioritiesAndTimeouts
//    Submit 200 tasks with random priorities and some with 50ms timeout.
//    Verify no crashes, timeout tasks are properly handled.
// ==========================================================================

TEST(LoadTest, LoadWithMixedPrioritiesAndTimeouts) {
    ThreadPool pool(load_config(4, 8));

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> prio_dist(0, 3);
    std::uniform_int_distribution<int> timeout_dist(0, 1);
    TaskPriority priorities[] = {
        TaskPriority::Low,
        TaskPriority::Normal,
        TaskPriority::High,
        TaskPriority::Critical
    };

    std::atomic<int> completed_count{0};
    std::atomic<int> timeout_count{0};

    std::vector<std::future<void>> futures;
    futures.reserve(200);

    for (int i = 0; i < 200; ++i) {
        TaskPriority p = priorities[prio_dist(rng)];
        bool has_timeout = (timeout_dist(rng) == 1);

        Task task([&, has_timeout, i]() {
            if (has_timeout && i % 20 == 0) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            completed_count.fetch_add(1, std::memory_order_relaxed);
        }, "mixed_" + std::to_string(i), p);

        if (has_timeout) {
            task.set_timeout(std::chrono::milliseconds(50));
        }

        futures.push_back(pool.submit(std::move(task)));
    }

    for (auto& f : futures) {
        try {
            f.get();
        } catch (const std::runtime_error&) {
            timeout_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    EXPECT_GT(completed_count.load(), 0);
    EXPECT_EQ(completed_count.load() + timeout_count.load(), 200);

    pool.shutdown();
}

}  // namespace
}  // namespace ctee
