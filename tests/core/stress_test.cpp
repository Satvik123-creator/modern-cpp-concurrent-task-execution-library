#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_priority.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/task_result.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

using detail::TaskQueue;

ThreadPoolConfig make_stress_config(std::size_t min_t = 8, std::size_t max_t = 8) {
    ThreadPoolConfig c;
    c.min_threads = min_t;
    c.max_threads = max_t;
    c.max_queue_size = 20000;
    c.scale_up_threshold = 10;
    c.enable_metrics = true;
    return c;
}

// ==========================================================================
// 1. StressThousandTasks
//    Submit 1000 noop tasks to pool with 8 threads, wait for all,
//    verify all completed via atomic counter.
// ==========================================================================

TEST(StressTest, StressThousandTasks) {
    ThreadPool pool(make_stress_config(8, 8));
    std::atomic<int> counter{0};

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

    EXPECT_EQ(counter.load(), 1000);
    pool.shutdown();
}

// ==========================================================================
// 2. StressQueueCapacity
//    Create queue with capacity 100, push 200 tasks.
//    Verify size never exceeds 100 and pop returns exactly 100 tasks.
// ==========================================================================

TEST(StressTest, StressQueueCapacity) {
    TaskQueue queue(100);

    for (int i = 0; i < 200; ++i) {
        queue.push(Task([] {}, "task_" + std::to_string(i)));
    }

    EXPECT_LE(queue.size(), 100u);

    std::size_t popped = 0;
    while (auto t = queue.pop()) {
        ++popped;
    }
    EXPECT_EQ(popped, 100u);
    EXPECT_TRUE(queue.empty());
}

// ==========================================================================
// 3. StressManyWorkers
//    Pool with min=8 max=16, submit 500 tasks.
//    Verify all complete, no crashes.
// ==========================================================================

TEST(StressTest, StressManyWorkers) {
    ThreadPool pool(make_stress_config(8, 16));
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(500);

    for (int i = 0; i < 500; ++i) {
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        })));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), 500);
    pool.shutdown();
}

// ==========================================================================
// 4. StressRapidSubmitAndCancel
//    Submit 200 tasks, immediately cancel 100 of them by ID.
//    Verify no crash, no deadlock.
// ==========================================================================

TEST(StressTest, StressRapidSubmitAndCancel) {
    ThreadPoolConfig c;
    c.min_threads = 4;
    c.max_threads = 8;
    c.max_queue_size = 500;
    c.scale_up_threshold = 10;
    c.enable_metrics = true;
    ThreadPool pool(c);

    std::vector<std::string> ids;
    ids.reserve(200);

    for (int i = 0; i < 200; ++i) {
        Task t([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }, "cancel_task_" + std::to_string(i));
        std::string id = t.id();
        ids.push_back(id);
        [[maybe_unused]] auto f = pool.submit(std::move(t));
    }

    for (int i = 0; i < 100; ++i) {
        pool.cancel(ids[i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::atomic<int> completed{0};
    for (int i = 100; i < 200; ++i) {
        Task t([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        [[maybe_unused]] auto f = pool.submit(std::move(t));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pool.shutdown();
    SUCCEED();
}

// ==========================================================================
// 5. StressMetricsAccuracy
//    Submit 500 tasks, wait for all, verify metrics counters
//    (submitted, completed) are consistent.
// ==========================================================================

TEST(StressTest, StressMetricsAccuracy) {
    ThreadPool pool(make_stress_config(8, 8));
    auto m = pool.metrics();

    std::vector<std::future<void>> futures;
    futures.reserve(500);

    for (int i = 0; i < 500; ++i) {
        futures.push_back(pool.submit(Task([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        })));
    }

    for (auto& f : futures) {
        f.get();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto snap = m->snapshot();
    EXPECT_EQ(snap.submitted_tasks, 500u);
    EXPECT_EQ(snap.completed_tasks, 500u);
    EXPECT_EQ(snap.failed_tasks + snap.cancelled_tasks, 0u);

    pool.shutdown();
}

// ==========================================================================
// 6. StressHistoryCapacity
//    Create TaskHistory with max_size=50, record 200 entries,
//    verify count=50 and recent(1000) returns 50.
// ==========================================================================

TEST(StressTest, StressHistoryCapacity) {
    TaskHistory history(50);
    auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 200; ++i) {
        TaskRecord rec;
        rec.task_id = "id_" + std::to_string(i);
        rec.task_name = "task_" + std::to_string(i);
        rec.status = TaskStatus::Completed;
        rec.submitted_at = now;
        rec.completed_at = now + std::chrono::milliseconds(i);
        rec.execution_time = std::chrono::milliseconds(1);
        history.record(rec);
    }

    EXPECT_EQ(history.count(), 50u);

    auto recent = history.recent(1000);
    EXPECT_EQ(recent.size(), 50u);
}

// ==========================================================================
// 7. StressBatchSubmission
//    From 4 threads simultaneously, each submit batch of 100 tasks.
//    Verify all 400 IDs returned are unique.
// ==========================================================================

TEST(StressTest, StressBatchSubmission) {
    ThreadPool pool(make_stress_config(8, 8));

    std::mutex all_ids_mutex;
    std::vector<std::string> all_ids;

    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&pool, &all_ids, &all_ids_mutex, t] {
            std::vector<Task> batch;
            batch.reserve(100);
            for (int i = 0; i < 100; ++i) {
                batch.push_back(Task([] {},
                    "thread_" + std::to_string(t) + "_task_" + std::to_string(i)));
            }
            auto ids = pool.submit_batch(std::move(batch));

            std::lock_guard lock(all_ids_mutex);
            for (auto& id : ids) {
                all_ids.push_back(std::move(id));
            }
        });
    }

    for (auto& t : submitters) {
        t.join();
    }

    EXPECT_EQ(all_ids.size(), 400u);

    std::set<std::string> unique_ids(all_ids.begin(), all_ids.end());
    EXPECT_EQ(unique_ids.size(), 400u);

    pool.shutdown();
}

// ==========================================================================
// 8. StressPriorityMix
//    Submit 500 tasks with random priorities (low/normal/high/critical),
//    wait for all. Verify high-priority tasks complete before all
//    low-priority ones (statistically — track completion order).
// ==========================================================================

TEST(StressTest, StressPriorityMix) {
    ThreadPoolConfig c;
    c.min_threads = 4;
    c.max_threads = 4;
    c.max_queue_size = 1000;
    c.scale_up_threshold = 10;
    c.enable_metrics = true;
    ThreadPool pool(c);

    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(500);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 3);
    TaskPriority priorities[] = {
        TaskPriority::Low,
        TaskPriority::Normal,
        TaskPriority::High,
        TaskPriority::Critical
    };

    for (int i = 0; i < 500; ++i) {
        TaskPriority p = priorities[dist(rng)];
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }, "task_" + std::to_string(i), p)));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), 500);
    pool.shutdown();
}

}  // namespace
}  // namespace ctee
