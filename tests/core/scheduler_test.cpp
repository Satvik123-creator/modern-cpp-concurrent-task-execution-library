#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "ctee/core/scheduler.h"
#include "ctee/core/task.h"
#include "ctee/core/thread_pool.h"

namespace ctee {
namespace {

using namespace std::chrono_literals;

ThreadPoolConfig make_pool(std::size_t threads) {
    ThreadPoolConfig c;
    c.min_threads = threads;
    c.max_threads = threads;
    c.max_queue_size = 100;
    return c;
}

TEST(SchedulerTest, Construction) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);
    EXPECT_TRUE(scheduler.is_running());
    EXPECT_FALSE(scheduler.is_paused());
    EXPECT_EQ(scheduler.scheduled_count(), 0u);
}

TEST(SchedulerTest, ShutdownIsIdempotent) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);
    scheduler.shutdown();
    scheduler.shutdown();
    EXPECT_FALSE(scheduler.is_running());
}

TEST(SchedulerTest, DestructorCallsShutdown) {
    ThreadPool pool(make_pool(1));
    {
        Scheduler scheduler(pool);
        EXPECT_TRUE(scheduler.is_running());
    }
    EXPECT_TRUE(pool.is_running());
}

TEST(SchedulerTest, ScheduleNowExecutesImmediately) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};
    scheduler.schedule_now(Task([&] { executed = true; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(executed.load());
}

TEST(SchedulerTest, ScheduleNowReturnsTaskId) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    auto id = scheduler.schedule_now(Task([] {}));
    EXPECT_FALSE(id.empty());
}

TEST(SchedulerTest, DelayedTaskExecutesAfterDelay) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    auto start = std::chrono::steady_clock::now();
    std::atomic<bool> executed{false};

    scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 100ms
    );

    std::this_thread::sleep_for(200ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(executed.load());
    EXPECT_GE(elapsed, 100ms);
}

TEST(SchedulerTest, DelayedTaskNotExecutedEarly) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};

    scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 500ms
    );

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(executed.load());
}

TEST(SchedulerTest, MultipleDelayedTasksExecuteInOrder) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::vector<int> order;
    std::mutex m;

    scheduler.schedule(
        Task([&] { std::lock_guard l(m); order.push_back(1); }),
        std::chrono::steady_clock::now() + 150ms
    );
    scheduler.schedule(
        Task([&] { std::lock_guard l(m); order.push_back(2); }),
        std::chrono::steady_clock::now() + 50ms
    );
    scheduler.schedule(
        Task([&] { std::lock_guard l(m); order.push_back(3); }),
        std::chrono::steady_clock::now() + 100ms
    );

    std::this_thread::sleep_for(300ms);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 3);
    EXPECT_EQ(order[2], 1);
}

TEST(SchedulerTest, ScheduledCountTracksHeapSize) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    EXPECT_EQ(scheduler.scheduled_count(), 0u);

    scheduler.schedule(
        Task([] {}),
        std::chrono::steady_clock::now() + 1h
    );
    EXPECT_EQ(scheduler.scheduled_count(), 1u);

    scheduler.schedule(
        Task([] {}),
        std::chrono::steady_clock::now() + 1h
    );
    EXPECT_EQ(scheduler.scheduled_count(), 2u);
}

TEST(SchedulerTest, RecurringTaskExecutesMultipleTimes) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    scheduler.schedule_recurring(
        Task([&] { count++; }),
        50ms
    );

    std::this_thread::sleep_for(300ms);
    EXPECT_GE(count.load(), 3);
}

TEST(SchedulerTest, RecurringTaskRespectsMaxRepeats) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    scheduler.schedule_recurring(
        Task([&] { count++; }),
        30ms,
        3
    );

    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(count.load(), 3);
}

TEST(SchedulerTest, RecurringTaskStopsAfterMaxRepeats) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    scheduler.schedule_recurring(
        Task([&] { count++; }),
        30ms,
        2
    );

    std::this_thread::sleep_for(100ms);
    std::this_thread::sleep_for(200ms);
    auto final_count = count.load();

    EXPECT_LE(final_count, 3);
}

TEST(SchedulerTest, CancelBeforeExecution) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};

    scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 500ms
    );

    scheduler.cancel("");
    std::this_thread::sleep_for(100ms);
}

TEST(SchedulerTest, CancelById) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};

    auto id = scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 500ms
    );

    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(scheduler.cancel(id));

    std::this_thread::sleep_for(600ms);
    EXPECT_FALSE(executed.load());
}

TEST(SchedulerTest, CancelRecurringTask) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    auto id = scheduler.schedule_recurring(
        Task([&] { count++; }),
        30ms
    );

    std::this_thread::sleep_for(150ms);
    auto before = count.load();

    scheduler.cancel(id);
    std::this_thread::sleep_for(200ms);
    auto after = count.load();

    EXPECT_LE(after - before, 1);
}

TEST(SchedulerTest, CancelNonexistentTask) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    EXPECT_TRUE(scheduler.cancel("nonexistent-id"));
}

TEST(SchedulerTest, RescheduleEarlier) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};
    auto start = std::chrono::steady_clock::now();

    auto id = scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 2s
    );

    std::this_thread::sleep_for(20ms);

    EXPECT_TRUE(scheduler.reschedule(id, std::chrono::steady_clock::now() + 50ms));

    std::this_thread::sleep_for(200ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(executed.load());
    EXPECT_LT(elapsed, 500ms);
}

TEST(SchedulerTest, RescheduleLater) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};

    auto id = scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 50ms
    );

    EXPECT_TRUE(scheduler.reschedule(id, std::chrono::steady_clock::now() + 500ms));

    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(executed.load());

    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(executed.load());
}

TEST(SchedulerTest, RescheduleNonexistentTask) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    EXPECT_FALSE(scheduler.reschedule("fake-id", std::chrono::steady_clock::now() + 1s));
}

TEST(SchedulerTest, PausePreventsExecution) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::atomic<bool> executed{false};

    scheduler.schedule(
        Task([&] { executed = true; }),
        std::chrono::steady_clock::now() + 50ms
    );

    scheduler.pause();
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(executed.load());

    scheduler.resume();
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(executed.load());
}

TEST(SchedulerTest, PauseResumePreservesTasks) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    scheduler.schedule_recurring(
        Task([&] { count++; }),
        50ms
    );

    std::this_thread::sleep_for(130ms);
    auto before_pause = count.load();

    scheduler.pause();
    std::this_thread::sleep_for(100ms);
    auto during_pause = count.load();

    scheduler.resume();
    std::this_thread::sleep_for(150ms);
    auto after_resume = count.load();

    EXPECT_EQ(before_pause, during_pause);
    EXPECT_GT(after_resume, during_pause);
}

TEST(SchedulerTest, ImmediateTaskDoesNotIncrementScheduledCount) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    scheduler.schedule_now(Task([] {}));
    EXPECT_EQ(scheduler.scheduled_count(), 0u);
}

TEST(SchedulerTest, EmptySchedulerShutdown) {
    ThreadPool pool(make_pool(1));
    Scheduler scheduler(pool);

    scheduler.shutdown();
    EXPECT_FALSE(scheduler.is_running());
}

TEST(SchedulerTest, ManyDelayedTasks) {
    ThreadPool pool(make_pool(4));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    for (int i = 0; i < 50; ++i) {
        scheduler.schedule(
            Task([&] { count++; }),
            std::chrono::steady_clock::now() + std::chrono::milliseconds(20 + i * 5)
        );
    }

    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(count.load(), 50);
}

TEST(SchedulerTest, RecurringWithZeroInterval) {
    ThreadPool pool(make_pool(2));
    Scheduler scheduler(pool);

    std::atomic<int> count{0};

    scheduler.schedule_recurring(
        Task([&] { count++; }),
        0ms,
        5
    );

    std::this_thread::sleep_for(200ms);
    EXPECT_GE(count.load(), 5);
}

}  // namespace
}  // namespace ctee
