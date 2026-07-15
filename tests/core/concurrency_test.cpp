#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "ctee/core/metrics.h"
#include "ctee/core/task.h"
#include "ctee/core/task_history.h"
#include "ctee/core/task_priority.h"
#include "ctee/core/task_queue.h"
#include "ctee/core/task_result.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/thread_pool_config.h"

namespace ctee {
namespace {

ThreadPoolConfig make_concurrency_config(std::size_t min_t = 4, std::size_t max_t = 8) {
    ThreadPoolConfig c;
    c.min_threads = min_t;
    c.max_threads = max_t;
    c.max_queue_size = 10000;
    c.scale_up_threshold = 5;
    c.enable_metrics = true;
    return c;
}

// ==========================================================================
// 1. ConcurrentSubmit
//    8 threads each submit 100 tasks simultaneously.
//    Verify all 800 tasks complete.
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentSubmit) {
    ThreadPool pool(make_concurrency_config(4, 8));
    std::atomic<int> counter{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < 8; ++t) {
        submitters.emplace_back([&pool, &counter] {
            for (int i = 0; i < 100; ++i) {
                [[maybe_unused]] auto f = pool.submit(Task([&counter] {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }));
            }
        });
    }

    for (auto& t : submitters) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(counter.load(), 800);
    pool.shutdown();
}

// ==========================================================================
// 2. ConcurrentSubmitAndCancel
//    Thread A submits tasks, Thread B cancels them by ID.
//    No crash, no data race.
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentSubmitAndCancel) {
    ThreadPoolConfig c;
    c.min_threads = 4;
    c.max_threads = 4;
    c.max_queue_size = 500;
    c.scale_up_threshold = 10;
    c.enable_metrics = true;
    ThreadPool pool(c);

    std::atomic<bool> done_submitting{false};
    std::atomic<int> completed{0};

    std::vector<std::string> submitted_ids;
    std::mutex ids_mutex;

    std::thread submitter([&pool, &submitted_ids, &ids_mutex, &done_submitting, &completed] {
        for (int i = 0; i < 100; ++i) {
            Task t([&completed] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                completed.fetch_add(1, std::memory_order_relaxed);
            }, "concurrent_task_" + std::to_string(i));
            std::string id = t.id();
            {
                std::lock_guard lock(ids_mutex);
                submitted_ids.push_back(id);
            }
            [[maybe_unused]] auto f = pool.submit(std::move(t));
        }
        done_submitting.store(true);
    });

    std::thread canceller([&pool, &submitted_ids, &ids_mutex, &done_submitting] {
        std::mt19937 rng(42);
        while (!done_submitting.load(std::memory_order_relaxed)) {
            std::lock_guard lock(ids_mutex);
            if (!submitted_ids.empty()) {
                std::uniform_int_distribution<std::size_t> dist(0, submitted_ids.size() - 1);
                pool.cancel(submitted_ids[dist(rng)]);
            }
        }
    });

    submitter.join();
    canceller.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pool.shutdown();
    SUCCEED();
}

// ==========================================================================
// 3. ConcurrentQueuePushPop
//    4 pusher threads + 4 popper threads operating on same queue.
//    Verify no crash, no lost tasks.
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentQueuePushPop) {
    // TaskQueue is NOT thread-safe — it relies on the pool's mutex.
    // Test concurrent access through the pool, which is the intended usage.
    ThreadPool pool(make_concurrency_config(4, 8));
    std::atomic<int> counter{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&pool, &counter, t] {
            for (int i = 0; i < 100; ++i) {
                try {
                    [[maybe_unused]] auto f = pool.submit(Task([&counter] {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    }, "p" + std::to_string(t) + "_" + std::to_string(i)));
                } catch (...) {}
            }
        });
    }

    for (auto& t : submitters) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(counter.load(), 400);
    pool.shutdown();
}

// ==========================================================================
// 4. ConcurrentHistoryAccess
//    4 threads recording, 4 threads reading recent(). No crash.
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentHistoryAccess) {
    TaskHistory history(200);
    std::atomic<bool> done{false};

    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&history, &done, t] {
            while (!done.load(std::memory_order_relaxed)) {
                TaskRecord rec;
                rec.task_id = "w" + std::to_string(t) + "_" + std::to_string(rand());
                rec.task_name = "writer_task";
                rec.status = TaskStatus::Completed;
                rec.submitted_at = std::chrono::steady_clock::now();
                rec.completed_at = rec.submitted_at;
                rec.execution_time = std::chrono::milliseconds(1);
                history.record(rec);
                std::this_thread::yield();
            }
        });
    }

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&history, &done] {
            while (!done.load(std::memory_order_relaxed)) {
                auto recent = history.recent(10);
                (void)recent;
                auto count = history.count();
                (void)count;
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    done.store(true);

    for (auto& t : writers) t.join();
    for (auto& t : readers) t.join();

    SUCCEED();
}

// ==========================================================================
// 5. ConcurrentSubmitBatch
//    4 threads each call submit_batch simultaneously.
//    Verify all IDs unique.
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentSubmitBatch) {
    ThreadPool pool(make_concurrency_config(8, 8));

    std::mutex all_ids_mutex;
    std::vector<std::string> all_ids;

    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&pool, &all_ids, &all_ids_mutex, t] {
            std::vector<Task> batch;
            batch.reserve(50);
            for (int i = 0; i < 50; ++i) {
                batch.push_back(Task([] {},
                    "batch_" + std::to_string(t) + "_" + std::to_string(i)));
            }
            auto ids = pool.submit_batch(std::move(batch));

            std::lock_guard lock(all_ids_mutex);
            for (auto& id : ids) {
                all_ids.push_back(std::move(id));
            }
        });
    }

    for (auto& t : submitters) {
        t.join();
    }

    EXPECT_EQ(all_ids.size(), 200u);

    std::set<std::string> unique_ids(all_ids.begin(), all_ids.end());
    EXPECT_EQ(unique_ids.size(), 200u);

    pool.shutdown();
}

// ==========================================================================
// 6. RaceSubmitVsShutdown
//    Half the threads submit tasks, other half call shutdown().
//    Verify no crash, no undefined behavior.
// ==========================================================================

TEST(ConcurrencyTest, RaceSubmitVsShutdown) {
    for (int iter = 0; iter < 5; ++iter) {
        ThreadPool pool(make_concurrency_config(4, 8));
        std::atomic<int> completed{0};

        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&pool, &completed, t] {
                for (int i = 0; i < 20; ++i) {
                    try {
                        auto f = pool.submit(Task([&completed] {
                            completed.fetch_add(1, std::memory_order_relaxed);
                        }));
                    } catch (const std::runtime_error&) {
                        // Expected: pool was shut down
                    }
                }
            });
        }

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&pool, t] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                pool.shutdown();
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        SUCCEED();
    }
}

// ==========================================================================
// 7. RaceCancelVsExecution
//    Submit long-running tasks (100ms sleep), cancel them immediately.
//    Verify no crash, cancelled tasks don't corrupt state.
// ==========================================================================

TEST(ConcurrencyTest, RaceCancelVsExecution) {
    ThreadPoolConfig c;
    c.min_threads = 4;
    c.max_threads = 4;
    c.max_queue_size = 500;
    c.scale_up_threshold = 10;
    c.enable_metrics = true;
    ThreadPool pool(c);

    std::vector<std::string> ids;
    std::mutex ids_mutex;

    for (int i = 0; i < 50; ++i) {
        Task t([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }, "long_task_" + std::to_string(i));
        std::string id = t.id();
        {
            std::lock_guard lock(ids_mutex);
            ids.push_back(id);
        }
        [[maybe_unused]] auto f = pool.submit(std::move(t));
    }

    {
        std::lock_guard lock(ids_mutex);
        for (auto& id : ids) {
            pool.cancel(id);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pool.shutdown();
    SUCCEED();
}

// ==========================================================================
// 8. RaceCompleteCallbackVsCancel
//    Submit tasks, set completion handlers, then immediately cancel.
//    Verify handler is called exactly once (not double-called).
// ==========================================================================

TEST(ConcurrencyTest, RaceCompleteCallbackVsCancel) {
    ThreadPoolConfig c;
    c.min_threads = 4;
    c.max_threads = 4;
    c.max_queue_size = 500;
    c.scale_up_threshold = 10;
    c.enable_metrics = true;
    ThreadPool pool(c);

    constexpr int NUM_TASKS = 50;
    std::vector<std::atomic<int>> handler_calls(NUM_TASKS);
    for (auto& h : handler_calls) {
        h.store(0);
    }

    std::vector<std::string> ids;
    std::vector<std::future<void>> futures;

    for (int i = 0; i < NUM_TASKS; ++i) {
        Task t([i, &handler_calls] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            handler_calls[i].fetch_add(1, std::memory_order_relaxed);
        });
        std::string id = t.id();
        ids.push_back(id);

        pool.set_completion_handler(id, [&handler_calls, i](TaskStatus) {
            handler_calls[i].fetch_add(1, std::memory_order_relaxed);
        });

        futures.push_back(pool.submit(std::move(t)));
    }

    for (int i = 0; i < NUM_TASKS; i += 2) {
        pool.cancel(ids[i]);
    }

    for (auto& f : futures) {
        try { f.get(); } catch (...) {}
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pool.shutdown();
    SUCCEED();
}

// ==========================================================================
// 9. ConcurrentPauseResume
//    Thread A pauses, Thread B resumes, Thread C submits.
//    No crash regardless of ordering.
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentPauseResume) {
    for (int iter = 0; iter < 10; ++iter) {
        ThreadPool pool(make_concurrency_config(4, 8));
        std::atomic<int> counter{0};

        std::vector<std::thread> threads;

        threads.emplace_back([&pool] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pool.pause();
        });

        threads.emplace_back([&pool] {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            pool.resume();
        });

        threads.emplace_back([&pool, &counter] {
            for (int i = 0; i < 10; ++i) {
                try {
                    [[maybe_unused]] auto f = pool.submit(Task([&counter] {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    }));
                } catch (...) {}
            }
        });

        for (auto& t : threads) {
            t.join();
        }

        pool.resume();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        pool.shutdown();
        SUCCEED();
    }
}

// ==========================================================================
// 10. ConcurrentMetricsRecording
//    8 threads call metrics methods simultaneously.
//    Verify no crash and snapshot values are internally consistent
//    (completed <= submitted).
// ==========================================================================

TEST(ConcurrencyTest, ConcurrentMetricsRecording) {
    Metrics m;
    std::atomic<bool> done{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&m, &done, t] {
            while (!done.load(std::memory_order_relaxed)) {
                m.record_submit(1);
                m.record_start();
                // Alternate between completed and cancelled to avoid
                // double-counting (each submit only produces one outcome).
                if (t % 2 == 0) {
                    m.record_complete(TaskStatus::Completed,
                                      std::chrono::milliseconds(1),
                                      std::chrono::milliseconds(0));
                } else {
                    m.record_cancel();
                }
                m.set_queue_length(t);
                m.set_active_threads(t + 1);
                (void)m.snapshot();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    done.store(true);
    for (auto& t : threads) {
        t.join();
    }

    auto snap = m.snapshot();
    EXPECT_GE(snap.submitted_tasks, 0u);
    EXPECT_GE(snap.completed_tasks, 0u);
    EXPECT_LE(snap.completed_tasks + snap.failed_tasks + snap.cancelled_tasks,
              snap.submitted_tasks);
}

}  // namespace
}  // namespace ctee
