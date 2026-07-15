#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

ThreadPoolConfig retry_config() {
    ThreadPoolConfig c;
    c.min_threads = 2;
    c.max_threads = 4;
    c.max_queue_size = 100;
    return c;
}

// ==========================================================================
// Retry tests
// ==========================================================================

TEST(RetryTest, RetriesOnFailure) {
    ThreadPool pool(retry_config());

    std::atomic<int> attempts{0};
    Task task([&attempts]() {
        attempts++;
        if (attempts.load() < 3) {
            throw std::runtime_error("fail");
        }
    });
    task.set_max_retries(3);

    auto future = pool.submit(std::move(task));
    future.get();

    EXPECT_EQ(attempts.load(), 3);
}

TEST(RetryTest, NoRetryWhenDisabled) {
    ThreadPool pool(retry_config());

    std::atomic<int> attempts{0};
    Task task([&attempts]() {
        attempts++;
        throw std::runtime_error("always fails");
    });
    task.set_max_retries(0);

    auto future = pool.submit(std::move(task));
    EXPECT_THROW(future.get(), std::runtime_error);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(attempts.load(), 1);
}

TEST(RetryTest, RetriesExhausted) {
    ThreadPool pool(retry_config());

    std::atomic<int> attempts{0};
    Task task([&attempts]() {
        attempts++;
        throw std::runtime_error("always fails");
    });
    task.set_max_retries(2);

    auto future = pool.submit(std::move(task));
    EXPECT_THROW(future.get(), std::runtime_error);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(attempts.load(), 3);
}

TEST(RetryTest, NoRetryOnSuccess) {
    ThreadPool pool(retry_config());

    std::atomic<int> attempts{0};
    Task task([&attempts]() {
        attempts++;
    });
    task.set_max_retries(3);

    auto future = pool.submit(std::move(task));
    future.get();

    EXPECT_EQ(attempts.load(), 1);
}

TEST(RetryTest, RetryPreservesTaskName) {
    ThreadPool pool(retry_config());

    std::atomic<int> attempts{0};
    Task task([&attempts]() {
        attempts++;
        if (attempts.load() < 2) {
            throw std::runtime_error("fail");
        }
    }, "named_task", TaskPriority::High);
    task.set_max_retries(3);

    auto future = pool.submit(std::move(task));
    future.get();

    EXPECT_EQ(attempts.load(), 2);

    auto records = pool.task_history()->recent(10);
    bool found = false;
    for (const auto& rec : records) {
        if (rec.task_name == "named_task" &&
            rec.status == TaskStatus::Completed) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RetryTest, TaskWithTimeoutCompletesNormally) {
    ThreadPool pool(retry_config());

    std::atomic<int> attempts{0};
    Task task([&attempts]() {
        attempts++;
    });
    task.set_max_retries(2);
    task.set_timeout(std::chrono::seconds(5));

    auto future = pool.submit(std::move(task));
    future.get();

    EXPECT_EQ(attempts.load(), 1);
}

}  // namespace
}  // namespace ctee
