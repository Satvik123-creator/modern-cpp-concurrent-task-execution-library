#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "ctee/core/task_queue.h"
#include "ctee/core/thread_pool.h"

namespace ctee {
namespace {

using detail::TaskQueue;

// ==========================================================================
// Group 1: Basic push / pop
// ==========================================================================

TEST(TaskQueueTest, PushAndPop) {
    TaskQueue queue(10);
    queue.push(Task([] {}, "test"));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size(), 1u);
    auto popped = queue.pop();
    EXPECT_TRUE(popped.has_value());
    EXPECT_TRUE(queue.empty());
}

TEST(TaskQueueTest, PopEmptyReturnsNullopt) {
    TaskQueue queue(10);
    auto result = queue.pop();
    EXPECT_FALSE(result.has_value());
}

// ==========================================================================
// Group 2: Priority ordering
// ==========================================================================

TEST(TaskQueueTest, PriorityOrdering) {
    TaskQueue queue(10);
    queue.push(Task([] {}, "low", TaskPriority::Low));
    queue.push(Task([] {}, "critical", TaskPriority::Critical));
    queue.push(Task([] {}, "normal", TaskPriority::Normal));

    auto t1 = queue.pop();
    auto t2 = queue.pop();
    auto t3 = queue.pop();

    EXPECT_TRUE(t1.has_value());
    EXPECT_TRUE(t2.has_value());
    EXPECT_TRUE(t3.has_value());
    EXPECT_EQ(t1->name(), "critical");
    EXPECT_EQ(t2->name(), "normal");
    EXPECT_EQ(t3->name(), "low");
}

TEST(TaskQueueTest, HigherPriorityPrecedesLower) {
    // Push in reverse order: Low, Normal, High, Critical.
    // Pop order should be Critical, High, Normal, Low.
    TaskQueue queue(10);
    queue.push(Task([] {}, "low", TaskPriority::Low));
    queue.push(Task([] {}, "normal", TaskPriority::Normal));
    queue.push(Task([] {}, "high", TaskPriority::High));
    queue.push(Task([] {}, "critical", TaskPriority::Critical));

    EXPECT_EQ(queue.pop()->name(), "critical");
    EXPECT_EQ(queue.pop()->name(), "high");
    EXPECT_EQ(queue.pop()->name(), "normal");
    EXPECT_EQ(queue.pop()->name(), "low");
}

// ==========================================================================
// Group 3: FIFO within same priority
// ==========================================================================

TEST(TaskQueueTest, FifoWithinSamePriority) {
    TaskQueue queue(10);

    // Push 5 tasks with Normal priority.  They should come out in the
    // same order they were pushed (FIFO).
    for (int i = 0; i < 5; ++i) {
        // Small sleep ensures distinct timestamps.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        queue.push(Task([] {}, "task_" + std::to_string(i), TaskPriority::Normal));
    }

    for (int i = 0; i < 5; ++i) {
        auto t = queue.pop();
        ASSERT_TRUE(t.has_value());
        EXPECT_EQ(t->name(), "task_" + std::to_string(i))
            << "Task " << i << " should pop first (FIFO)";
    }
}

TEST(TaskQueueTest, FifoAcrossMultiplePriorities) {
    // Interleave tasks of different priorities, all pushed with tiny
    // delays so timestamps are strictly ordered.
    TaskQueue queue(20);

    queue.push(Task([] {}, "L1", TaskPriority::Low));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    queue.push(Task([] {}, "H1", TaskPriority::High));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    queue.push(Task([] {}, "L2", TaskPriority::Low));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    queue.push(Task([] {}, "H2", TaskPriority::High));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    queue.push(Task([] {}, "N1", TaskPriority::Normal));

    // Pop order: H1, H2 (higher priority, FIFO among themselves),
    //            N1,
    //            L1, L2
    EXPECT_EQ(queue.pop()->name(), "H1");
    EXPECT_EQ(queue.pop()->name(), "H2");
    EXPECT_EQ(queue.pop()->name(), "N1");
    EXPECT_EQ(queue.pop()->name(), "L1");
    EXPECT_EQ(queue.pop()->name(), "L2");
}

// ==========================================================================
// Group 4: Mixed priorities and timestamps stress test
// ==========================================================================

TEST(TaskQueueTest, StressManyTasks) {
    constexpr int N = 200;
    TaskQueue queue(N + 10);

    // Push tasks in reverse priority order with staggered timestamps.
    for (int i = N - 1; i >= 0; --i) {
        TaskPriority p = static_cast<TaskPriority>(i % 4);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        queue.push(Task([] {}, "t" + std::to_string(i), p));
    }

    EXPECT_EQ(queue.size(), static_cast<std::size_t>(N));

    // Pop all and verify: higher priority first, then FIFO within level.
    // Tasks with the same priority were pushed in decreasing sequence
    // order (t199 first, then t195, ...), so FIFO means decreasing seq.
    TaskPriority prev = TaskPriority::Critical;
    int prev_seq = -1;
    std::string prev_name;

    bool first = true;
    while (auto t = queue.pop()) {
        if (!first) {
            if (t->priority() == prev) {
                int seq = std::stoi(t->name().substr(1));
                EXPECT_LT(seq, prev_seq)
                    << "FIFO violated within same priority band";
            } else {
                EXPECT_LT(static_cast<uint8_t>(t->priority()),
                          static_cast<uint8_t>(prev))
                    << "Priority ordering violated";
            }
        }
        prev = t->priority();
        prev_name = t->name();
        prev_seq = std::stoi(prev_name.substr(1));
        first = false;
    }
}

// ==========================================================================
// Group 5: All same priority → pure FIFO
// ==========================================================================

TEST(TaskQueueTest, AllSamePriorityIsFifo) {
    constexpr int N = 50;
    TaskQueue queue(N + 5);

    for (int i = 0; i < N; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        queue.push(Task([] {}, "seq_" + std::to_string(i), TaskPriority::Normal));
    }

    for (int i = 0; i < N; ++i) {
        auto t = queue.pop();
        ASSERT_TRUE(t.has_value());
        EXPECT_EQ(t->name(), "seq_" + std::to_string(i));
    }
}

// ==========================================================================
// Group 6: Higher priority arrives late → still goes first
// ==========================================================================

TEST(TaskQueueTest, LateHighPriorityJumpsAhead) {
    TaskQueue queue(10);

    // Fill with Low tasks.
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        queue.push(Task([] {}, "low_" + std::to_string(i), TaskPriority::Low));
    }

    // Now push a Critical task.
    queue.push(Task([] {}, "critical_first", TaskPriority::Critical));

    // Critical should come out first, before all the Lows.
    EXPECT_EQ(queue.pop()->name(), "critical_first");

    // Then the Lows in FIFO order.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(queue.pop()->name(), "low_" + std::to_string(i));
    }
}

// ==========================================================================
// Group 7: Cancel
// ==========================================================================

TEST(TaskQueueTest, CancelTask) {
    TaskQueue queue(10);
    Task task([] {}, "test");
    std::string id = task.id();
    queue.push(std::move(task));
    queue.cancel(id);
    EXPECT_TRUE(queue.is_cancelled(id));
    auto popped = queue.pop();
    EXPECT_FALSE(popped.has_value());
}

TEST(TaskQueueTest, CancelMiddleTask) {
    TaskQueue queue(10);
    queue.push(Task([] {}, "first", TaskPriority::Normal));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Task middle([] {}, "middle", TaskPriority::Normal);
    std::string middle_id = middle.id();
    queue.push(std::move(middle));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    queue.push(Task([] {}, "last", TaskPriority::Normal));

    queue.cancel(middle_id);

    auto t1 = queue.pop();
    auto t2 = queue.pop();
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ(t1->name(), "first");
    EXPECT_EQ(t2->name(), "last");
}

TEST(TaskQueueTest, CancelNonexistentId) {
    TaskQueue queue(10);
    // Cancelling an ID that was never pushed should not crash.
    EXPECT_TRUE(queue.cancel("nonexistent"));
}

// ==========================================================================
// Group 8: Capacity
// ==========================================================================

TEST(TaskQueueTest, CapacityLimit) {
    TaskQueue queue(2);
    queue.push(Task([] {}));
    queue.push(Task([] {}));
    queue.push(Task([] {}));
    EXPECT_EQ(queue.size(), 2u);
}

TEST(TaskQueueTest, IsFull) {
    TaskQueue queue(3);
    EXPECT_FALSE(queue.is_full());
    queue.push(Task([] {}));
    EXPECT_FALSE(queue.is_full());
    queue.push(Task([] {}));
    EXPECT_FALSE(queue.is_full());
    queue.push(Task([] {}));
    EXPECT_TRUE(queue.is_full());
}

// ==========================================================================
// Group 9: Clear
// ==========================================================================

TEST(TaskQueueTest, Clear) {
    TaskQueue queue(10);
    queue.push(Task([] {}));
    queue.push(Task([] {}));
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
}

// ==========================================================================
// Group 10: Priority with ThreadPool integration
// ==========================================================================

TEST(TaskQueuePriorityIntegration, PriorityWithThreadPool) {
    // This test uses the actual ThreadPool to verify that the priority
    // queue produces correct execution ordering under concurrency.
    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.max_queue_size = 100;
    ThreadPool pool(config);

    std::vector<std::string> execution_order;

    // Submit a long-running Low task to occupy the single worker.
    [[maybe_unused]] auto f0 = pool.submit(Task([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }, "blocking_low", TaskPriority::Low));

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Now submit High and Normal tasks.  They queue behind the running
    // Low task, but when it finishes, High should execute before Normal.
    auto f1 = pool.submit(Task([&execution_order] {
        execution_order.push_back("high");
    }, "high", TaskPriority::High));

    auto f2 = pool.submit(Task([&execution_order] {
        execution_order.push_back("normal");
    }, "normal", TaskPriority::Normal));

    f1.get();
    f2.get();

    ASSERT_EQ(execution_order.size(), 2u);
    EXPECT_EQ(execution_order[0], "high");
    EXPECT_EQ(execution_order[1], "normal");
}

}  // namespace
}  // namespace ctee
