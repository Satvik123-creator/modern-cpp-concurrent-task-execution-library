#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/thread_pool.h"

namespace ctee {
namespace {

// ==========================================================================
// Group 1: Basic construction
// ==========================================================================

TEST(TaskTest, ConstructionWithFunc) {
    bool executed = false;
    Task task([&] { executed = true; }, "test_task", TaskPriority::High);
    EXPECT_EQ(task.name(), "test_task");
    EXPECT_EQ(task.priority(), TaskPriority::High);
    EXPECT_EQ(task.status(), TaskStatus::Pending);
    EXPECT_FALSE(task.id().empty());
}

TEST(TaskTest, ExecutesViaPool) {
    bool executed = false;
    Task task([&] { executed = true; }, "exec_test", TaskPriority::Normal);

    ThreadPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.max_queue_size = 10;
    ThreadPool pool(cfg);
    auto f = pool.submit(std::move(task));
    f.get();
    EXPECT_TRUE(executed);
}

TEST(TaskTest, ExceptionHandlingViaPool) {
    Task task([] { throw std::runtime_error("test error"); }, "err_test");

    ThreadPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.max_queue_size = 10;
    ThreadPool pool(cfg);
    auto f = pool.submit(std::move(task));
    EXPECT_THROW(f.get(), std::runtime_error);
}

// ==========================================================================
// Group 2: operator< comparator
// ==========================================================================

TEST(TaskComparator, HigherPriorityIsGreater) {
    Task low([] {}, "low", TaskPriority::Low);
    Task high([] {}, "high", TaskPriority::High);

    // low < high means low has lower effective priority.
    // In a max-heap, high is popped first.
    EXPECT_TRUE(low < high);
    EXPECT_FALSE(high < low);
}

TEST(TaskComparator, SamePriorityEarlierTimestampIsGreater) {
    // Two tasks with same priority: the one created earlier should be
    // popped first (FIFO).  "Earlier created" means it sits higher in
    // the max-heap, so earlier < later must be FALSE and later < earlier
    // must be TRUE.
    Task first([] {}, "first", TaskPriority::Normal);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Task second([] {}, "second", TaskPriority::Normal);

    // second was created later -> second < first (second goes below first)
    EXPECT_TRUE(second < first);
    EXPECT_FALSE(first < second);
}

TEST(TaskComparator, SamePrioritySameTimestampTiebreaksOnId) {
    Task a([] {}, "a", TaskPriority::Normal);
    Task b([] {}, "b", TaskPriority::Normal);

    // They may or may not have the same timestamp (depends on speed).
    // Either way, exactly one of a < b or b < a is true (strict weak).
    bool ab = (a < b);
    bool ba = (b < a);
    EXPECT_NE(ab, ba);  // exactly one direction holds
}

TEST(TaskComparator, AllPriorityLevelsOrderCorrectly) {
    Task critical([] {}, "crit", TaskPriority::Critical);
    Task high([] {}, "high", TaskPriority::High);
    Task normal([] {}, "norm", TaskPriority::Normal);
    Task low([] {}, "low", TaskPriority::Low);

    // Verify the full chain: Critical > High > Normal > Low
    EXPECT_TRUE(low < normal);
    EXPECT_TRUE(normal < high);
    EXPECT_TRUE(high < critical);

    // Transitivity
    EXPECT_TRUE(low < critical);
}

// ==========================================================================
// Group 3: Move semantics
// ==========================================================================

TEST(TaskTest, MoveConstruction) {
    Task original([] {}, "movable", TaskPriority::High);
    std::string original_id = original.id();

    Task moved(std::move(original));
    EXPECT_EQ(moved.id(), original_id);
    EXPECT_EQ(moved.priority(), TaskPriority::High);
}

TEST(TaskTest, MoveAssignment) {
    Task original([] {}, "movable", TaskPriority::Low);
    std::string original_id = original.id();

    Task target([] {}, "dummy", TaskPriority::Normal);
    target = std::move(original);
    EXPECT_EQ(target.id(), original_id);
    EXPECT_EQ(target.priority(), TaskPriority::Low);
}

// ==========================================================================
// Group 4: Unique IDs
// ==========================================================================

TEST(TaskTest, UniqueIds) {
    std::vector<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        Task t([] {}, "t" + std::to_string(i));
        ids.push_back(t.id());
    }
    // All 100 UUIDs should be distinct.
    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    EXPECT_EQ(last, ids.end());
}

}  // namespace
}  // namespace ctee
