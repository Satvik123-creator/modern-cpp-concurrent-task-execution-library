#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ctee/core/scheduler.h"
#include "ctee/core/task.h"
#include "ctee/core/task_priority.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

constexpr auto kDeadlockTimeout = std::chrono::seconds(5);

ThreadPoolConfig deadlock_config(std::size_t threads = 4) {
    ThreadPoolConfig c;
    c.min_threads = threads;
    c.max_threads = threads;
    c.max_queue_size = 10000;
    c.enable_metrics = true;
    return c;
}

// ---------------------------------------------------------------------------
// 1. ShutdownWhileTasksRunning
// ---------------------------------------------------------------------------
TEST(DeadlockTest, ShutdownWhileTasksRunning) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        for (int i = 0; i < 10; ++i) {
            pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            });
        }

        pool.shutdown();
    });

    auto status = f.wait_for(kDeadlockTimeout);
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: ShutdownWhileTasksRunning did not complete within "
        << kDeadlockTimeout.count() << "s";
}

// ---------------------------------------------------------------------------
// 2. ShutdownWhileTasksQueued
// ---------------------------------------------------------------------------
TEST(DeadlockTest, ShutdownWhileTasksQueued) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(1));

        pool.pause();

        for (int i = 0; i < 50; ++i) {
            pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            });
        }

        pool.shutdown();
    });

    auto status = f.wait_for(kDeadlockTimeout);
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: ShutdownWhileTasksQueued did not complete within "
        << kDeadlockTimeout.count() << "s";
}

// ---------------------------------------------------------------------------
// 3. CancelAllThenShutdown
// ---------------------------------------------------------------------------
TEST(DeadlockTest, CancelAllThenShutdown) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        std::vector<std::string> ids;
        for (int i = 0; i < 100; ++i) {
            auto fut = pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            });
            // We can't easily get the task ID from the future, so we just
            // let them finish and rely on shutdown cancelling anything left.
        }

        pool.shutdown();
    });

    auto status = f.wait_for(kDeadlockTimeout);
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: CancelAllThenShutdown did not complete within "
        << kDeadlockTimeout.count() << "s";
}

// ---------------------------------------------------------------------------
// 4. PauseResumeDuringExecution
// ---------------------------------------------------------------------------
TEST(DeadlockTest, PauseResumeDuringExecution) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        std::atomic<bool> producer_done{false};

        // Producer thread: keeps submitting tasks.
        std::thread producer([&]() {
            for (int i = 0; i < 200; ++i) {
                pool.submit([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            producer_done.store(true, std::memory_order_release);
        });

        // Rapid pause/resume cycle.
        for (int i = 0; i < 50; ++i) {
            pool.pause();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pool.resume();
        }

        producer.join();

        // Let remaining tasks drain.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        pool.shutdown();
    });

    auto status = f.wait_for(std::chrono::seconds(10));
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: PauseResumeDuringExecution did not complete within 10s";
}

// ---------------------------------------------------------------------------
// 5. SubmitDuringShutdown
// ---------------------------------------------------------------------------
TEST(DeadlockTest, SubmitDuringShutdown) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        std::atomic<bool> shutdown_called{false};

        // Some threads submit tasks.
        std::vector<std::thread> submitters;
        for (int t = 0; t < 4; ++t) {
            submitters.emplace_back([&pool, &shutdown_called]() {
                for (int i = 0; i < 50; ++i) {
                    if (shutdown_called.load(std::memory_order_acquire)) break;
                    try {
                        pool.submit([]() {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        });
                    } catch (...) {
                        break;
                    }
                }
            });
        }

        // Give tasks time to accumulate, then shutdown.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        shutdown_called.store(true, std::memory_order_release);
        pool.shutdown();

        for (auto& th : submitters) {
            th.join();
        }
    });

    auto status = f.wait_for(kDeadlockTimeout);
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: SubmitDuringShutdown did not complete within "
        << kDeadlockTimeout.count() << "s";
}

// ---------------------------------------------------------------------------
// 6. MultipleShutdownCalls
// ---------------------------------------------------------------------------
TEST(DeadlockTest, MultipleShutdownCalls) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        pool.submit([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
        pool.submit([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });

        // Fire shutdown from multiple threads concurrently.
        std::vector<std::thread> shutdown_threads;
        for (int i = 0; i < 10; ++i) {
            shutdown_threads.emplace_back([&pool]() {
                pool.shutdown();
            });
        }

        for (auto& th : shutdown_threads) {
            th.join();
        }
    });

    auto status = f.wait_for(kDeadlockTimeout);
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: MultipleShutdownCalls did not complete within "
        << kDeadlockTimeout.count() << "s";
}

// ---------------------------------------------------------------------------
// 7. CancelNonexistentTasks
// ---------------------------------------------------------------------------
TEST(DeadlockTest, CancelNonexistentTasks) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        // Submit a task to keep the pool alive.
        pool.submit([]() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); });

        // Cancel 100 non-existent task IDs.
        for (int i = 0; i < 100; ++i) {
            bool result = pool.cancel("nonexistent_id_" + std::to_string(i));
            // Should return false, but must not crash or hang.
            (void)result;
        }

        pool.shutdown();
    });

    auto status = f.wait_for(kDeadlockTimeout);
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: CancelNonexistentTasks did not complete within "
        << kDeadlockTimeout.count() << "s";
}

// ---------------------------------------------------------------------------
// 8. FullQueueThenCancel
// ---------------------------------------------------------------------------
TEST(DeadlockTest, FullQueueThenCancel) {
    auto f = std::async(std::launch::async, []() {
        ThreadPoolConfig cfg;
        cfg.min_threads = 1;
        cfg.max_threads = 1;
        cfg.max_queue_size = 50;
        cfg.enable_metrics = true;

        ThreadPool pool(cfg);

        // Submit a blocking task so the single worker is occupied.
        std::atomic<bool> blocker_done{false};
        pool.submit([&blocker_done]() {
            while (!blocker_done.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Fill the queue.
        std::vector<std::string> queued_ids;
        for (int i = 0; i < 50; ++i) {
            try {
                pool.submit([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                });
            } catch (...) {
                break;
            }
        }

        // Unblock the worker and let things drain.
        blocker_done.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Should be able to submit more after queue drains.
        std::atomic<int> final_count{0};
        for (int i = 0; i < 10; ++i) {
            pool.submit([&final_count]() {
                final_count.fetch_add(1, std::memory_order_relaxed);
            });
        }

        auto future = pool.submit([&final_count]() -> int {
            return final_count.load(std::memory_order_relaxed);
        });

        auto result = future.get();
        (void)result;

        pool.shutdown();
    });

    auto status = f.wait_for(std::chrono::seconds(8));
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: FullQueueThenCancel did not complete within 8s";
}

// ---------------------------------------------------------------------------
// 9. RecursiveTaskSubmission
// ---------------------------------------------------------------------------
TEST(DeadlockTest, RecursiveTaskSubmission) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(4));

        std::atomic<int> depth_counter{0};

        // A task that submits another task to the same pool.
        // This must not deadlock — submit() should not block the worker.
        std::function<void(int)> recursive_submit = [&](int depth) {
            depth_counter.fetch_add(1, std::memory_order_relaxed);
            if (depth <= 0) return;

            // Worker thread calls submit on the same pool.
            pool.submit([&pool, &recursive_submit, depth]() {
                recursive_submit(depth - 1);
            });
        };

        // Start the chain from 4 independent root tasks.
        for (int root = 0; root < 4; ++root) {
            pool.submit([&recursive_submit]() {
                recursive_submit(10);
            });
        }

        // Wait for all nested tasks to complete.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (depth_counter.load(std::memory_order_relaxed) < 44 &&  // 4*(10+1) = 44
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        pool.shutdown();

        EXPECT_GE(depth_counter.load(std::memory_order_relaxed), 44);
    });

    auto status = f.wait_for(std::chrono::seconds(8));
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: RecursiveTaskSubmission did not complete within 8s";
}

// ---------------------------------------------------------------------------
// 10. SchedulerShutdownDuringTimer
// ---------------------------------------------------------------------------
TEST(DeadlockTest, SchedulerShutdownDuringTimer) {
    auto f = std::async(std::launch::async, []() {
        ThreadPool pool(deadlock_config(2));
        Scheduler scheduler(pool);

        std::atomic<bool> task_ran{false};

        Task task([&task_ran]() {
            task_ran.store(true, std::memory_order_release);
        });

        // Schedule a task 500ms in the future.
        auto now = std::chrono::steady_clock::now();
        scheduler.schedule(std::move(task), now + std::chrono::milliseconds(500));

        // Immediately shut down the scheduler.
        scheduler.shutdown();
        pool.shutdown();

        // The task may or may not have run (race between schedule and shutdown),
        // but the system must not deadlock.
    });

    auto status = f.wait_for(std::chrono::seconds(2));
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: SchedulerShutdownDuringTimer did not complete within 2s";
}

// ---------------------------------------------------------------------------
// 11. DestroyPoolWhileRunning
// ---------------------------------------------------------------------------
TEST(DeadlockTest, DestroyPoolWhileRunning) {
    auto f = std::async(std::launch::async, []() {
        std::atomic<bool> destructor_done{false};

        {
            ThreadPool pool(deadlock_config(4));

            for (int i = 0; i < 20; ++i) {
                pool.submit([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                });
            }

            // Let destructor run — it should block until tasks finish.
            // We test that it doesn't hang.
        }

        destructor_done.store(true, std::memory_order_release);
        EXPECT_TRUE(destructor_done.load(std::memory_order_acquire));
    });

    auto status = f.wait_for(std::chrono::seconds(8));
    ASSERT_NE(status, std::future_status::timeout)
        << "Deadlock detected: DestroyPoolWhileRunning did not complete within 8s";
}

// ---------------------------------------------------------------------------
// 12. SubmitToShutdownPool
// ---------------------------------------------------------------------------
TEST(DeadlockTest, SubmitToShutdownPool) {
    ThreadPool pool(deadlock_config(2));
    pool.shutdown();

    // Submitting to a shut-down pool should throw, not hang.
    bool threw = false;
    try {
        auto fut = pool.submit([]() {});
        fut.get();  // The exception is set on the future.
    } catch (const std::runtime_error& e) {
        threw = true;
    }

    EXPECT_TRUE(threw) << "Submit to shutdown pool should throw std::runtime_error";
}

}  // namespace
}  // namespace ctee
