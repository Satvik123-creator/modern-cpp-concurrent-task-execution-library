#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

ThreadPoolConfig batch_config() {
    ThreadPoolConfig c;
    c.min_threads = 4;
    c.max_threads = 8;
    c.max_queue_size = 100;
    return c;
}

// ==========================================================================
// Batch tests
// ==========================================================================

TEST(BatchTest, SubmitMultipleTasks) {
    ThreadPool pool(batch_config());

    std::atomic<int> count{0};
    std::vector<Task> tasks;
    for (int i = 0; i < 5; i++) {
        tasks.push_back(Task([&count]() { count++; }));
    }

    auto ids = pool.submit_batch(std::move(tasks));
    EXPECT_EQ(ids.size(), 5u);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(count.load(), 5);
}

TEST(BatchTest, SubmitEmptyBatch) {
    ThreadPool pool(batch_config());

    std::vector<Task> tasks;
    auto ids = pool.submit_batch(std::move(tasks));
    EXPECT_TRUE(ids.empty());
}

TEST(BatchTest, BatchIdsAreUnique) {
    ThreadPool pool(batch_config());

    std::vector<Task> tasks;
    for (int i = 0; i < 10; i++) {
        tasks.push_back(Task([]() {}));
    }

    auto ids = pool.submit_batch(std::move(tasks));
    EXPECT_EQ(ids.size(), 10u);

    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    EXPECT_EQ(last, ids.end());
}

}  // namespace
}  // namespace ctee
