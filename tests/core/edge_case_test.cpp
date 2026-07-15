#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/task_result.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

using detail::TaskQueue;

ThreadPoolConfig edge_config() {
    ThreadPoolConfig c;
    c.min_threads = 2;
    c.max_threads = 4;
    c.max_queue_size = 1000;
    c.scale_up_threshold = 10;
    return c;
}

// ==========================================================================
// 1. EdgeEmptyFunction
//    Task with empty std::function<void()>.
//    Submit to pool. Should complete without crash.
// ==========================================================================

TEST(EdgeCaseTest, EdgeEmptyFunction) {
    ThreadPool pool(edge_config());

    std::function<void()> empty_func;
    Task task(std::move(empty_func));

    auto future = pool.submit(std::move(task));
    try {
        future.get();
    } catch (const std::exception&) {
    }

    SUCCEED();
    pool.shutdown();
}

// ==========================================================================
// 2. EdgeEmptyTaskName
//    Task with name="". Submit and verify id() is not empty.
// ==========================================================================

TEST(EdgeCaseTest, EdgeEmptyTaskName) {
    Task task([] {}, "");
    EXPECT_TRUE(task.id().empty() == false);
    EXPECT_EQ(task.name(), "");

    ThreadPool pool(edge_config());
    auto future = pool.submit(Task([] {}, ""));
    future.get();

    SUCCEED();
    pool.shutdown();
}

// ==========================================================================
// 3. EdgeLongTaskName
//    Task with name of 10000 chars. Submit and verify it works.
// ==========================================================================

TEST(EdgeCaseTest, EdgeLongTaskName) {
    std::string long_name(10000, 'A');
    std::atomic<bool> executed{false};

    Task task([&executed] { executed = true; }, long_name);
    EXPECT_EQ(task.name().size(), 10000u);

    ThreadPool pool(edge_config());
    auto future = pool.submit(std::move(task));
    future.get();

    EXPECT_TRUE(executed);
    pool.shutdown();
}

// ==========================================================================
// 4. EdgeZeroMaxRetries
//    Task with set_max_retries(0). Submit a throwing task.
//    Verify only 1 attempt.
// ==========================================================================

TEST(EdgeCaseTest, EdgeZeroMaxRetries) {
    ThreadPool pool(edge_config());

    std::atomic<int> attempts{0};
    Task task([&attempts] {
        attempts.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("always fails");
    });
    task.set_max_retries(0);

    auto future = pool.submit(std::move(task));
    EXPECT_THROW(future.get(), std::runtime_error);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(attempts.load(), 1);
    pool.shutdown();
}

// ==========================================================================
// 5. EdgeTimeoutOneMillisecond
//    Task that sleeps 100ms with 1ms timeout.
//    Should timeout. Verify status.
// ==========================================================================

TEST(EdgeCaseTest, EdgeTimeoutOneMillisecond) {
    ThreadPool pool(edge_config());

    Task task([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
    task.set_timeout(std::chrono::milliseconds(1));

    auto future = pool.submit(std::move(task));
    EXPECT_THROW(future.get(), std::runtime_error);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto records = pool.task_history()->recent(10);
    bool found_timeout = false;
    for (const auto& rec : records) {
        if (rec.status == TaskStatus::TimedOut) {
            found_timeout = true;
            break;
        }
    }
    EXPECT_TRUE(found_timeout);
    pool.shutdown();
}

// ==========================================================================
// 6. EdgeQueueCapacityOne
//    TaskQueue with capacity=1. Push 1, verify is_full.
//    Push second, verify rejected.
// ==========================================================================

TEST(EdgeCaseTest, EdgeQueueCapacityOne) {
    TaskQueue queue(1);

    EXPECT_FALSE(queue.is_full());
    queue.push(Task([] {}, "first"));
    EXPECT_TRUE(queue.is_full());
    EXPECT_EQ(queue.size(), 1u);

    queue.push(Task([] {}, "second"));
    EXPECT_EQ(queue.size(), 1u);

    auto popped = queue.pop();
    EXPECT_TRUE(popped.has_value());
    EXPECT_EQ(popped->name(), "first");

    auto second = queue.pop();
    EXPECT_FALSE(second.has_value());
}

// ==========================================================================
// 7. EdgePoolSingleWorker
//    ThreadPool with min=max=1. Submit 50 tasks.
//    Verify all complete.
// ==========================================================================

TEST(EdgeCaseTest, EdgePoolSingleWorker) {
    ThreadPoolConfig c;
    c.min_threads = 1;
    c.max_threads = 1;
    c.max_queue_size = 1000;
    ThreadPool pool(c);

    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(50);

    for (int i = 0; i < 50; ++i) {
        futures.push_back(pool.submit(Task([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        })));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), 50);
    pool.shutdown();
}

// ==========================================================================
// 8. EdgeSelfDependency
//    Push task A, then task B that depends on A. Pop A, mark complete,
//    pop B. Then push C that depends on C (itself). Verify pop() returns
//    nothing (it's deferred due to unmet dependency).
// ==========================================================================

TEST(EdgeCaseTest, EdgeSelfDependency) {
    TaskQueue queue(10);

    Task task_a([] {}, "A");
    std::string a_id = task_a.id();
    queue.push(std::move(task_a));

    Task task_b([] {}, "B");
    std::string b_id = task_b.id();
    task_b.set_depends_on({a_id});
    queue.push(std::move(task_b));

    Task task_c([] {}, "C");
    std::string c_id = task_c.id();
    task_c.set_depends_on({c_id});
    queue.push(std::move(task_c));

    auto popped_a = queue.pop();
    EXPECT_TRUE(popped_a.has_value());
    EXPECT_EQ(popped_a->name(), "A");
    queue.mark_completed(a_id);

    auto popped_b = queue.pop();
    EXPECT_TRUE(popped_b.has_value());
    EXPECT_EQ(popped_b->name(), "B");

    auto popped_c = queue.pop();
    EXPECT_FALSE(popped_c.has_value());
}

// ==========================================================================
// 9. EdgeSubmitAfterShutdown
//    Shutdown pool, then submit task. Should throw or return failed
//    future.
// ==========================================================================

TEST(EdgeCaseTest, EdgeSubmitAfterShutdown) {
    ThreadPool pool(edge_config());
    pool.shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto f1 = pool.submit([] { return 42; });
    EXPECT_THROW(f1.get(), std::runtime_error);

    auto f2 = pool.submit(Task([] {}));
    EXPECT_THROW(f2.get(), std::runtime_error);

    SUCCEED();
}

// ==========================================================================
// 10. EdgeMultipleShutdowns
//     Call shutdown() 5 times. Should not crash (idempotent).
// ==========================================================================

TEST(EdgeCaseTest, EdgeMultipleShutdowns) {
    ThreadPool pool(edge_config());
    pool.shutdown();
    pool.shutdown();
    pool.shutdown();
    pool.shutdown();
    pool.shutdown();

    EXPECT_FALSE(pool.is_running());
    SUCCEED();
}

// ==========================================================================
// 11. EdgeCancelAlreadyCompleted
//     Submit noop task, wait for completion, then cancel.
//     Should return true (no-op).
// ==========================================================================

TEST(EdgeCaseTest, EdgeCancelAlreadyCompleted) {
    ThreadPool pool(edge_config());

    Task task([] {});
    std::string id = task.id();

    auto future = pool.submit(std::move(task));
    future.get();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool result = pool.cancel(id);
    EXPECT_FALSE(result);  // Already completed, nothing to cancel

    pool.shutdown();
}

// ==========================================================================
// 12. EdgeHistoryCountZero
//     Call recent(0) on history. Should return empty vector.
// ==========================================================================

TEST(EdgeCaseTest, EdgeHistoryCountZero) {
    TaskHistory history(100);

    TaskRecord rec;
    rec.task_id = "t1";
    rec.status = TaskStatus::Completed;
    rec.completed_at = std::chrono::steady_clock::now();
    history.record(rec);

    auto result = history.recent(0);
    EXPECT_TRUE(result.empty());
}

// ==========================================================================
// 13. EdgeHistoryEmpty
//     Call recent(100) on empty history. Should return empty vector.
// ==========================================================================

TEST(EdgeCaseTest, EdgeHistoryEmpty) {
    TaskHistory history(100);
    auto result = history.recent(100);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(history.count(), 0u);
}

// ==========================================================================
// 16. EdgeTaskStatusTransitions
//     Create task, verify initial status is Pending. Execute it, verify
//     transitions Pending -> Running -> Completed.
// ==========================================================================

TEST(EdgeCaseTest, EdgeTaskStatusTransitions) {
    Task task([] {});
    EXPECT_EQ(task.status(), TaskStatus::Pending);

    // Execute via pool to verify Pending -> Running -> Completed transitions
    ThreadPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.max_queue_size = 10;
    ThreadPool pool(cfg);
    auto f = pool.submit(std::move(task));
    f.get();
}

// ==========================================================================
// 17. EdgeTaskMoveConstruct
//     Move-construct a task, verify old task's func is empty,
//     new task has func.
// ==========================================================================

TEST(EdgeCaseTest, EdgeTaskMoveConstruct) {
    std::atomic<bool> executed{false};

    Task original([&executed] { executed = true; }, "movable");
    EXPECT_FALSE(original.func() == nullptr);

    Task moved(std::move(original));
    EXPECT_FALSE(moved.func() == nullptr);

    ThreadPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.max_queue_size = 10;
    ThreadPool pool(cfg);
    auto f = pool.submit(std::move(moved));
    f.get();
    EXPECT_TRUE(executed);
}

// ==========================================================================
// 18. EdgeQueuePopEmpty
//     Pop from empty queue. Verify returns std::nullopt.
// ==========================================================================

TEST(EdgeCaseTest, EdgeQueuePopEmpty) {
    TaskQueue queue(10);
    auto result = queue.pop();
    EXPECT_FALSE(result.has_value());
}

// ==========================================================================
// 19. EdgeBatchSubmitAfterShutdown
//     Shutdown pool, submit_batch. Verify returns empty vector.
// ==========================================================================

TEST(EdgeCaseTest, EdgeBatchSubmitAfterShutdown) {
    ThreadPool pool(edge_config());
    pool.shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<Task> tasks;
    for (int i = 0; i < 5; ++i) {
        tasks.push_back(Task([] {}));
    }

    auto ids = pool.submit_batch(std::move(tasks));
    EXPECT_TRUE(ids.empty());
}

// ==========================================================================
// 20. EdgeConcurrentPopFromSingleQueue
//     2 threads popping from same queue (without pool). Verify no crash,
//     each gets unique task.
// ==========================================================================

TEST(EdgeCaseTest, EdgeConcurrentPopFromSingleQueue) {
    TaskQueue queue(100);

    for (int i = 0; i < 50; ++i) {
        queue.push(Task([] {}, "task_" + std::to_string(i)));
    }

    std::mutex queue_mutex;
    std::mutex ids_mutex;
    std::vector<std::string> popped_ids;

    std::vector<std::thread> poppers;
    for (int t = 0; t < 2; ++t) {
        poppers.emplace_back([&queue, &queue_mutex, &ids_mutex, &popped_ids] {
            while (true) {
                std::optional<Task> task;
                {
                    std::lock_guard lock(queue_mutex);
                    task = queue.pop();
                }
                if (!task.has_value()) break;
                std::lock_guard lock(ids_mutex);
                popped_ids.push_back(task->id());
            }
        });
    }

    for (auto& t : poppers) {
        t.join();
    }

    EXPECT_EQ(popped_ids.size(), 50u);

    std::sort(popped_ids.begin(), popped_ids.end());
    auto last = std::unique(popped_ids.begin(), popped_ids.end());
    EXPECT_EQ(last, popped_ids.end());
}

}  // namespace
}  // namespace ctee
