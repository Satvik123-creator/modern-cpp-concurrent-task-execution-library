#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_result.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

ThreadPoolConfig timeout_config() {
    ThreadPoolConfig c;
    c.min_threads = 2;
    c.max_threads = 4;
    c.max_queue_size = 100;
    return c;
}

// ==========================================================================
// Timeout tests
// ==========================================================================

TEST(TimeoutTest, TaskTimesOut) {
    // DESIGN: The worker runs the task on a detached thread and uses
    // shared_future::wait_for to enforce the deadline.  If the task
    // exceeds the timeout, on_complete is called with TimedOut status.
    // The future from pool.submit() should throw because on_task_complete
    // sets an exception on the void promise for failed/timed-out tasks.
    ThreadPool pool(timeout_config());

    Task task([]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
    });
    task.set_timeout(std::chrono::milliseconds(100));

    auto future = pool.submit(std::move(task));
    EXPECT_THROW(future.get(), std::runtime_error);

    // Verify the task appears as TimedOut in history.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto history = pool.task_history()->recent(10);
    bool found = false;
    for (const auto& rec : history) {
        if (rec.status == TaskStatus::TimedOut) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(TimeoutTest, TaskCompletesBeforeTimeout) {
    ThreadPool pool(timeout_config());

    Task task([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    task.set_timeout(std::chrono::seconds(5));

    auto future = pool.submit(std::move(task));
    future.get();  // should not throw
}

TEST(TimeoutTest, TaskStoresTimeoutValue) {
    Task task([]() {});
    EXPECT_EQ(task.timeout_ms().count(), 0);

    task.set_timeout(std::chrono::milliseconds(250));
    EXPECT_EQ(task.timeout_ms().count(), 250);
}

TEST(TimeoutTest, FastTaskWithGenerousTimeout) {
    ThreadPool pool(timeout_config());

    std::atomic<int> count{0};
    Task task([&count]() {
        count++;
    });
    task.set_timeout(std::chrono::seconds(10));

    auto future = pool.submit(std::move(task));
    future.get();

    EXPECT_EQ(count.load(), 1);
}

TEST(TimeoutTest, TaskRecordedInHistoryAfterCompletion) {
    ThreadPool pool(timeout_config());

    Task task([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }, "timeout_history_task");
    task.set_timeout(std::chrono::seconds(5));

    auto future = pool.submit(std::move(task));
    future.get();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto history = pool.task_history()->recent(10);
    bool found = false;
    for (const auto& rec : history) {
        if (rec.task_name == "timeout_history_task" &&
            rec.status == TaskStatus::Completed) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

}  // namespace
}  // namespace ctee
