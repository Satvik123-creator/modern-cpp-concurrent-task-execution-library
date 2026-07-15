#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "ctee/core/worker.h"
#include "ctee/core/task_queue.h"

namespace ctee {
namespace {

using detail::Worker;
using detail::TaskQueue;

class WorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue_.clear();
        pool_running_ = true;
        pool_paused_ = false;
    }

    std::mutex mutex_;
    std::condition_variable_any cv_;
    TaskQueue queue_{100};
    bool pool_running_ = true;
    bool pool_paused_ = false;
};

TEST_F(WorkerTest, InitialStateIsIdle) {
    Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
    EXPECT_EQ(w.state(), Worker::State::Idle);
}

TEST_F(WorkerTest, TransitionsToWaiting) {
    Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(w.state(), Worker::State::Waiting);
}

TEST_F(WorkerTest, ExecutesTask) {
    std::atomic<bool> executed{false};
    {
        Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        {
            std::lock_guard lock(mutex_);
            queue_.push(Task([&executed] { executed = true; }));
        }
        cv_.notify_one();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_TRUE(executed);
    }
}

TEST_F(WorkerTest, TransitionsToRunningDuringExecution) {
    std::atomic<bool> task_entered{false};
    std::atomic<bool> test_observed{false};
    std::promise<void> hold;
    auto hold_future = hold.get_future().share();

    {
        Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        {
            std::lock_guard lock(mutex_);
            queue_.push(Task([&] {
                task_entered = true;
                hold_future.wait();
            }));
        }
        cv_.notify_one();

        while (!task_entered.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        EXPECT_EQ(w.state(), Worker::State::Running);
        test_observed = true;

        hold.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(test_observed.load());
}

TEST_F(WorkerTest, ReturnsToWaitingAfterExecution) {
    Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    {
        std::lock_guard lock(mutex_);
        queue_.push(Task([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }));
    }
    cv_.notify_one();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(w.state(), Worker::State::Waiting);
}

TEST_F(WorkerTest, ExecutesMultipleTasks) {
    std::atomic<int> counter{0};
    {
        Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        for (int i = 0; i < 5; ++i) {
            {
                std::lock_guard lock(mutex_);
                queue_.push(Task([&counter] { counter++; }));
            }
            cv_.notify_one();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(counter.load(), 5);
    }
}

TEST_F(WorkerTest, RespectsPoolPausedState) {
    pool_paused_ = true;
    std::atomic<bool> executed{false};

    {
        Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        {
            std::lock_guard lock(mutex_);
            queue_.push(Task([&executed] { executed = true; }));
        }
        cv_.notify_one();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(executed);

        pool_paused_ = false;
        cv_.notify_all();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_TRUE(executed);
    }
}

TEST_F(WorkerTest, StopsOnPoolShutdown) {
    Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(w.state(), Worker::State::Waiting);

    {
        std::lock_guard lock(mutex_);
        pool_running_ = false;
    }
    cv_.notify_all();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(w.is_stopped());
}

TEST_F(WorkerTest, GracefulShutdownFinishesCurrentTask) {
    std::atomic<bool> finished{false};

    {
        Worker w(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        {
            std::lock_guard lock(mutex_);
            queue_.push(Task([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                finished = true;
            }));
        }
        cv_.notify_one();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        {
            std::lock_guard lock(mutex_);
            pool_running_ = false;
        }
        cv_.notify_all();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_TRUE(finished);
        EXPECT_TRUE(w.is_stopped());
    }
}

TEST_F(WorkerTest, UniqueIds) {
    Worker w1(0, mutex_, cv_, queue_, pool_running_, pool_paused_);
    Worker w2(1, mutex_, cv_, queue_, pool_running_, pool_paused_);
    Worker w3(42, mutex_, cv_, queue_, pool_running_, pool_paused_);

    EXPECT_EQ(w1.id(), 0u);
    EXPECT_EQ(w2.id(), 1u);
    EXPECT_EQ(w3.id(), 42u);
}

TEST_F(WorkerTest, StateToString) {
    EXPECT_STREQ(Worker::state_to_string(Worker::State::Idle), "idle");
    EXPECT_STREQ(Worker::state_to_string(Worker::State::Waiting), "waiting");
    EXPECT_STREQ(Worker::state_to_string(Worker::State::Running), "running");
    EXPECT_STREQ(Worker::state_to_string(Worker::State::Stopped), "stopped");
}

TEST_F(WorkerTest, MultipleWorkersExecuteConcurrently) {
    std::atomic<int> counter{0};

    {
        std::vector<std::unique_ptr<Worker>> workers;
        for (std::size_t i = 0; i < 4; ++i) {
            workers.push_back(std::make_unique<Worker>(
                i, mutex_, cv_, queue_, pool_running_, pool_paused_));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        for (int i = 0; i < 20; ++i) {
            {
                std::lock_guard lock(mutex_);
                queue_.push(Task([&counter] { counter++; }));
            }
            cv_.notify_one();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_EQ(counter.load(), 20);
    }
}

TEST_F(WorkerTest, ShutdownAllWorkers) {
    std::vector<std::unique_ptr<Worker>> workers;
    for (std::size_t i = 0; i < 4; ++i) {
        workers.push_back(std::make_unique<Worker>(
            i, mutex_, cv_, queue_, pool_running_, pool_paused_));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    {
        std::lock_guard lock(mutex_);
        pool_running_ = false;
    }
    cv_.notify_all();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto& w : workers) {
        EXPECT_TRUE(w->is_stopped());
    }
}

}  // namespace
}  // namespace ctee
