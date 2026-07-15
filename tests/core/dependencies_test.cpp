#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

using detail::TaskQueue;

ThreadPoolConfig dep_config() {
    ThreadPoolConfig c;
    c.min_threads = 2;
    c.max_threads = 4;
    c.max_queue_size = 100;
    c.scale_up_threshold = 50;
    return c;
}

// ==========================================================================
// Dependencies tests — via ThreadPool
// ==========================================================================

TEST(DependenciesTest, DependentTaskWaits) {
    ThreadPool pool(dep_config());

    std::atomic<int> order{0};
    int a_order = -1;
    int b_order = -1;

    Task task_a([&]() {
        a_order = order.fetch_add(1);
    });
    std::string a_id = task_a.id();

    Task task_b([&]() {
        b_order = order.fetch_add(1);
    });
    task_b.set_depends_on({a_id});

    auto fa = pool.submit(std::move(task_a));
    auto fb = pool.submit(std::move(task_b));

    fb.get();

    EXPECT_LT(a_order, b_order);
}

TEST(DependenciesTest, IndependentTasksRunFreely) {
    ThreadPool pool(dep_config());

    std::atomic<int> count{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; i++) {
        futures.push_back(pool.submit(Task([&count]() { count++; })));
    }

    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(count.load(), 10);
}

// ==========================================================================
// Dependencies tests — via TaskQueue directly
// ==========================================================================

TEST(DependenciesTest, MarkCompletedUnblocks) {
    TaskQueue queue(100);

    Task task_a([]() {});
    std::string a_id = task_a.id();

    Task task_b([]() {});
    std::string b_id = task_b.id();
    task_b.set_depends_on({a_id});

    queue.push(std::move(task_a));
    queue.push(std::move(task_b));

    auto popped = queue.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->id(), a_id);

    auto popped2 = queue.pop();
    EXPECT_FALSE(popped2.has_value());

    queue.mark_completed(a_id);

    auto popped3 = queue.pop();
    ASSERT_TRUE(popped3.has_value());
    EXPECT_EQ(popped3->id(), b_id);
}

TEST(DependenciesTest, MultipleDependencies) {
    TaskQueue queue(100);

    Task task_a([]() {});
    std::string a_id = task_a.id();

    Task task_b([]() {});
    std::string b_id = task_b.id();

    Task task_c([]() {});
    std::string c_id = task_c.id();
    task_c.set_depends_on({a_id, b_id});

    queue.push(std::move(task_a));
    queue.push(std::move(task_b));
    queue.push(std::move(task_c));

    auto p1 = queue.pop();
    ASSERT_TRUE(p1.has_value());
    queue.mark_completed(p1->id());

    auto p2 = queue.pop();
    ASSERT_TRUE(p2.has_value());
    EXPECT_NE(p2->id(), c_id);
    queue.mark_completed(p2->id());

    auto p3 = queue.pop();
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(p3->id(), c_id);
}

}  // namespace
}  // namespace ctee
