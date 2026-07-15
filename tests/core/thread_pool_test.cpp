#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "ctee/core/task.h"
#include "ctee/core/task_priority.h"
#include "ctee/core/thread_pool.h"

namespace ctee {
namespace {

ThreadPoolConfig make_config(std::size_t threads = 4) {
    ThreadPoolConfig c;
    c.min_threads = threads;
    c.max_threads = threads;
    c.max_queue_size = 100;
    return c;
}

// ----- helpers used across test groups ------------------------------------

int add(int a, int b) { return a + b; }

struct Accumulator {
    int sum = 0;
    void add_value(int v) { sum += v; }
};

struct Point {
    int x;
    int y;
    int manhattan() const { return x + y; }
};

struct Matrix {
    std::vector<std::vector<int>> data;
    std::size_t rows() const { return data.size(); }
    std::size_t cols() const { return data.empty() ? 0 : data[0].size(); }
    int trace() const {
        int t = 0;
        for (std::size_t i = 0; i < std::min(rows(), cols()); ++i)
            t += data[i][i];
        return t;
    }
};

// ==========================================================================
// Group 1: Construction / lifecycle
// ==========================================================================

TEST(ThreadPoolLifecycle, ConstructionStartsWorkers) {
    ThreadPool pool(make_config(4));
    EXPECT_TRUE(pool.is_running());
    EXPECT_FALSE(pool.is_paused());
    EXPECT_EQ(pool.thread_count(), 4u);
}

TEST(ThreadPoolLifecycle, DestructorJoinsAllThreads) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(make_config(2));
        for (int i = 0; i < 10; ++i) {
            [[maybe_unused]] auto f =
                pool.submit(Task([&counter] { counter++; }));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(counter.load(), 10);
}

TEST(ThreadPoolLifecycle, ShutdownIdempotent) {
    ThreadPool pool(make_config(2));
    pool.shutdown();
    pool.shutdown();
    pool.shutdown();
    EXPECT_FALSE(pool.is_running());
}

TEST(ThreadPoolLifecycle, ShutdownDrainsQueue) {
    ThreadPool pool(make_config(2));
    std::atomic<int> counter{0};

    for (int i = 0; i < 10; ++i) {
        [[maybe_unused]] auto f = pool.submit(Task([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            counter++;
        }));
    }

    pool.shutdown();
    EXPECT_FALSE(pool.is_running());
    EXPECT_EQ(counter.load(), 10);
}

// ==========================================================================
// Group 2: submit(F&&, Args&&...) – arbitrary callables
// ==========================================================================

TEST(ThreadPoolSubmitCallable, LambdaReturningValue) {
    ThreadPool pool(make_config(2));
    auto f = pool.submit([] { return 42; });
    EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPoolSubmitCallable, LambdaWithArguments) {
    ThreadPool pool(make_config(2));
    auto f = pool.submit([](int a, int b) { return a + b; }, 3, 7);
    EXPECT_EQ(f.get(), 10);
}

TEST(ThreadPoolSubmitCallable, FreeFunction) {
    ThreadPool pool(make_config(2));
    auto f = pool.submit(add, 3, 7);
    EXPECT_EQ(f.get(), 10);
}

TEST(ThreadPoolSubmitCallable, MemberFunction) {
    ThreadPool pool(make_config(2));
    Accumulator acc;
    auto f = pool.submit(&Accumulator::add_value, &acc, 5);
    f.get();
    EXPECT_EQ(acc.sum, 5);
}

TEST(ThreadPoolSubmitCallable, VoidLambda) {
    ThreadPool pool(make_config(2));
    std::atomic<int> counter{0};
    auto f = pool.submit([&counter] { counter++; });
    f.get();
    EXPECT_EQ(counter.load(), 1);
}

TEST(ThreadPoolSubmitCallable, MultipleTasks) {
    ThreadPool pool(make_config(4));
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 20; ++i) {
        futures.push_back(
            pool.submit(Task([&counter] { counter++; })));
    }
    for (auto& f : futures) f.get();
    EXPECT_EQ(counter.load(), 20);
}

TEST(ThreadPoolSubmitCallable, MultipleTemplateTasks) {
    ThreadPool pool(make_config(4));

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit(add, i, i));
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(futures[i].get(), i * 2);
    }
}

TEST(ThreadPoolSubmitCallable, AfterShutdownThrows) {
    ThreadPool pool(make_config(2));
    pool.shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto f = pool.submit([] { return 1; });
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(ThreadPoolSubmitCallable, ConcurrentSubmitFromMultipleThreads) {
    ThreadPool pool(make_config(4));
    std::atomic<int> counter{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&pool, &counter] {
            for (int i = 0; i < 25; ++i) {
                [[maybe_unused]] auto f =
                    pool.submit(Task([&counter] { counter++; }));
            }
        });
    }
    for (auto& t : submitters) t.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPoolSubmitCallable, ConcurrentTemplateSubmit) {
    ThreadPool pool(make_config(4));
    std::atomic<int> sum{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&pool, &sum] {
            for (int i = 0; i < 25; ++i) {
                auto f = pool.submit([](int v) { return v; }, 1);
                sum += f.get();
            }
        });
    }
    for (auto& t : submitters) t.join();
    EXPECT_EQ(sum.load(), 100);
}

// ==========================================================================
// Group 3: submit(Task) – legacy Task-based path
// ==========================================================================

TEST(ThreadPoolSubmitTask, ReturnsFuture) {
    ThreadPool pool(make_config(2));
    std::atomic<bool> executed{false};

    auto future = pool.submit(Task([&executed] { executed = true; }));
    future.get();
    EXPECT_TRUE(executed);
}

TEST(ThreadPoolSubmitTask, AfterShutdownThrows) {
    ThreadPool pool(make_config(2));
    pool.shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto future = pool.submit(Task([] {}));
    EXPECT_THROW(future.get(), std::runtime_error);
}

// ==========================================================================
// Group 4: submit(packaged_task) – pre-built packaged_task
// ==========================================================================

TEST(ThreadPoolPackagedTask, VoidPackagedTask) {
    ThreadPool pool(make_config(2));
    std::atomic<int> result{0};

    std::packaged_task<void()> pt([&result] { result = 99; });
    auto f = pool.submit(std::move(pt));
    f.get();

    EXPECT_EQ(result.load(), 99);
}

TEST(ThreadPoolPackagedTask, IntPackagedTask) {
    ThreadPool pool(make_config(2));

    std::packaged_task<int()> pt([] { return 42; });
    auto f = pool.submit(std::move(pt));
    EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPoolPackagedTask, StringPackagedTask) {
    ThreadPool pool(make_config(2));

    std::packaged_task<std::string()> pt([] {
        return std::string("hello from packaged_task");
    });
    auto f = pool.submit(std::move(pt));
    EXPECT_EQ(f.get(), "hello from packaged_task");
}

TEST(ThreadPoolPackagedTask, ExceptionInPackagedTask) {
    ThreadPool pool(make_config(2));

    std::packaged_task<int()> pt([]() -> int {
        throw std::logic_error("packaged_task error");
    });
    auto f = pool.submit(std::move(pt));
    EXPECT_THROW(f.get(), std::logic_error);
}

TEST(ThreadPoolPackagedTask, MultiplePackagedTasks) {
    ThreadPool pool(make_config(4));

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        // Bind the argument so the packaged_task signature is R().
        std::packaged_task<int()> pt([i] { return i * i; });
        futures.push_back(pool.submit(std::move(pt)));
    }

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(futures[i].get(), i * i);
    }
}

TEST(ThreadPoolPackagedTask, AfterShutdownReturnsBrokenPromise) {
    ThreadPool pool(make_config(2));
    pool.shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::packaged_task<int()> pt([] { return 1; });
    auto f = pool.submit(std::move(pt));

    // The packaged_task is destroyed when submit rejects it.
    // future.get() throws broken_promise because the shared state
    // is broken by the packaged_task destructor.
    EXPECT_THROW(f.get(), std::future_error);
}

// ==========================================================================
// Group 5: Return types – any return type through futures
// ==========================================================================

TEST(ThreadPoolReturnTypes, PrimitiveTypes) {
    ThreadPool pool(make_config(2));

    auto f_int    = pool.submit([] { return 42; });
    auto f_double = pool.submit([] { return 3.14; });
    auto f_char   = pool.submit([] { return 'A'; });
    auto f_bool   = pool.submit([] { return true; });
    auto f_uint   = pool.submit([]() -> unsigned { return 42u; });

    EXPECT_EQ(f_int.get(), 42);
    EXPECT_DOUBLE_EQ(f_double.get(), 3.14);
    EXPECT_EQ(f_char.get(), 'A');
    EXPECT_TRUE(f_bool.get());
    EXPECT_EQ(f_uint.get(), 42u);
}

TEST(ThreadPoolReturnTypes, StringType) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([] {
        return std::string("hello world");
    });
    EXPECT_EQ(f.get(), "hello world");
}

TEST(ThreadPoolReturnTypes, CustomStruct) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([] {
        return Point{3, 4};
    });
    Point p = f.get();
    EXPECT_EQ(p.x, 3);
    EXPECT_EQ(p.y, 4);
    EXPECT_EQ(p.manhattan(), 7);
}

TEST(ThreadPoolReturnTypes, MatrixType) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([] {
        Matrix m;
        m.data = {{1, 2}, {3, 4}};
        return m;
    });
    Matrix m = f.get();
    EXPECT_EQ(m.rows(), 2u);
    EXPECT_EQ(m.cols(), 2u);
    EXPECT_EQ(m.trace(), 5);
}

TEST(ThreadPoolReturnTypes, ContainerVector) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([] {
        std::vector<int> v(10);
        std::iota(v.begin(), v.end(), 1);
        return v;
    });
    std::vector<int> result = f.get();
    ASSERT_EQ(result.size(), 10u);
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[9], 10);
}

TEST(ThreadPoolReturnTypes, ContainerMap) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([] {
        std::map<std::string, int> m;
        m["one"] = 1;
        m["two"] = 2;
        return m;
    });
    std::map<std::string, int> result = f.get();
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result["one"], 1);
    EXPECT_EQ(result["two"], 2);
}

TEST(ThreadPoolReturnTypes, PairAndTuple) {
    ThreadPool pool(make_config(2));

    auto f1 = pool.submit([] {
        return std::make_pair(42, std::string("answer"));
    });
    auto f2 = pool.submit([] {
        return std::make_tuple(1, 2.0, std::string("three"));
    });

    auto [i, s] = f1.get();
    EXPECT_EQ(i, 42);
    EXPECT_EQ(s, "answer");

    auto [a, b, c] = f2.get();
    EXPECT_EQ(a, 1);
    EXPECT_DOUBLE_EQ(b, 2.0);
    EXPECT_EQ(c, "three");
}

TEST(ThreadPoolReturnTypes, MoveOnlyType) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([] {
        return std::make_unique<int>(42);
    });
    std::unique_ptr<int> result = f.get();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, 42);
}

TEST(ThreadPoolReturnTypes, VoidReturn) {
    ThreadPool pool(make_config(2));
    std::atomic<bool> called{false};

    auto f = pool.submit([&called] { called = true; });
    f.get();
    EXPECT_TRUE(called);
}

// ==========================================================================
// Group 6: Exceptions – propagation through futures
// ==========================================================================

TEST(ThreadPoolExceptions, RuntimeError) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([]() -> int {
        throw std::runtime_error("runtime");
    });
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(ThreadPoolExceptions, LogicError) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([]() -> int {
        throw std::logic_error("logic");
    });
    EXPECT_THROW(f.get(), std::logic_error);
}

TEST(ThreadPoolExceptions, CustomException) {
    struct MyError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    ThreadPool pool(make_config(2));

    auto f = pool.submit([]() -> int {
        throw MyError("custom");
    });
    EXPECT_THROW(f.get(), MyError);
}

TEST(ThreadPoolExceptions, ExceptionMessagePreserved) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([]() -> int {
        throw std::runtime_error("specific message");
    });

    try {
        f.get();
        FAIL() << "Expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "specific message");
    }
}

TEST(ThreadPoolExceptions, VoidTaskException) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit([]() {
        throw std::runtime_error("void task error");
    });
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(ThreadPoolExceptions, TaskObjectException) {
    ThreadPool pool(make_config(2));

    auto f = pool.submit(Task([] {
        throw std::runtime_error("task object error");
    }));
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(ThreadPoolExceptions, PackagedTaskException) {
    ThreadPool pool(make_config(2));

    std::packaged_task<int()> pt([]() -> int {
        throw std::runtime_error("packaged_task error");
    });
    auto f = pool.submit(std::move(pt));
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(ThreadPoolExceptions, MultipleFailingTasks) {
    ThreadPool pool(make_config(4));

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([](int n) -> int {
            if (n % 2 == 0)
                throw std::runtime_error("even");
            return n;
        }, i));
    }

    for (int i = 0; i < 10; ++i) {
        if (i % 2 == 0) {
            EXPECT_THROW(futures[i].get(), std::runtime_error);
        } else {
            EXPECT_EQ(futures[i].get(), i);
        }
    }
}

TEST(ThreadPoolExceptions, MixedSuccessAndFailure) {
    ThreadPool pool(make_config(2));

    auto f_ok = pool.submit([] { return 10; });
    auto f_fail = pool.submit([]() -> int {
        throw std::runtime_error("fail");
    });
    auto f_ok2 = pool.submit([] { return 20; });

    EXPECT_EQ(f_ok.get(), 10);
    EXPECT_THROW(f_fail.get(), std::runtime_error);
    EXPECT_EQ(f_ok2.get(), 20);
}

// ==========================================================================
// Group 7: std::promise – external control
// ==========================================================================

TEST(ThreadPoolPromise, ExternalPromiseControl) {
    ThreadPool pool(make_config(2));

    // Pattern: create a promise/future pair externally, pass the
    // set-value responsibility to a task running on the pool.
    auto promise = std::make_shared<std::promise<int>>();
    auto future = promise->get_future();

    [[maybe_unused]] auto f = pool.submit([promise] {
        // Simulate some computation.
        int result = 6 * 7;
        promise->set_value(result);
    });

    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolPromise, ExceptionViaExternalPromise) {
    ThreadPool pool(make_config(2));

    auto promise = std::make_shared<std::promise<int>>();
    auto future = promise->get_future();

    [[maybe_unused]] auto f = pool.submit([promise] {
        try {
            throw std::runtime_error("external error");
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });

    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(ThreadPoolPromise, MultipleExternalPromises) {
    ThreadPool pool(make_config(4));

    std::vector<std::future<int>> futures;
    std::vector<std::shared_ptr<std::promise<int>>> promises;

    for (int i = 0; i < 5; ++i) {
        auto p = std::make_shared<std::promise<int>>();
        futures.push_back(p->get_future());
        promises.push_back(p);

        [[maybe_unused]] auto f = pool.submit([p, i] {
            p->set_value(i * 10);
        });
    }

    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(futures[i].get(), i * 10);
    }
}

TEST(ThreadPoolPromise, PromiseWithDelay) {
    ThreadPool pool(make_config(1));

    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    [[maybe_unused]] auto f = pool.submit([promise] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        promise->set_value(std::string("delayed result"));
    });

    EXPECT_EQ(future.get(), "delayed result");
}

// ==========================================================================
// Group 8: Pause / resume
// ==========================================================================

TEST(ThreadPoolPauseResume, PauseBlocksNewExecution) {
    ThreadPool pool(make_config(2));
    std::atomic<int> counter{0};

    pool.pause();
    EXPECT_TRUE(pool.is_paused());

    for (int i = 0; i < 4; ++i) {
        [[maybe_unused]] auto f =
            pool.submit(Task([&counter] { counter++; }));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(counter.load(), 0);

    pool.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(counter.load(), 4);
}

TEST(ThreadPoolPauseResume, PauseResumeTemplateTasks) {
    ThreadPool pool(make_config(2));

    pool.pause();
    auto future = pool.submit([] { return 99; });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(0)),
              std::future_status::timeout);

    pool.resume();
    EXPECT_EQ(future.get(), 99);
}

TEST(ThreadPoolPauseResume, SubmitManyAfterPauseResume) {
    ThreadPool pool(make_config(2));
    std::atomic<int> counter{0};

    pool.pause();
    for (int i = 0; i < 10; ++i) {
        [[maybe_unused]] auto f =
            pool.submit(Task([&counter] { counter++; }));
    }
    EXPECT_EQ(counter.load(), 0);

    pool.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(counter.load(), 10);
}

// ==========================================================================
// Group 9: Cancel
// ==========================================================================

TEST(ThreadPoolCancel, CancelPendingTask) {
    ThreadPool pool(make_config(1));

    std::atomic<bool> executed{false};
    auto task = Task([&executed] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        executed = true;
    });
    std::string id = task.id();

    auto future = pool.submit(std::move(task));
    pool.cancel(id);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_THROW(future.get(), std::runtime_error);
}

// ==========================================================================
// Group 10: Stats
// ==========================================================================

TEST(ThreadPoolStats, StatsTrackingTask) {
    ThreadPool pool(make_config(2));

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit(Task([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        })));
    }
    for (auto& f : futures) f.get();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto s = pool.stats();
    EXPECT_EQ(s.completed_tasks, 5u);
    EXPECT_EQ(s.active_threads, 2u);
    EXPECT_EQ(s.pending_tasks, 0u);
}

TEST(ThreadPoolStats, StatsTrackingTemplateTasks) {
    ThreadPool pool(make_config(2));

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit(add, i, 0));
    }
    for (auto& f : futures) f.get();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto s = pool.stats();
    EXPECT_EQ(s.completed_tasks, 5u);
}

// ==========================================================================
// Group 11: Priority ordering
// ==========================================================================

TEST(ThreadPoolPriority, TaskPriorityOrdering) {
    ThreadPool pool(make_config(1));
    std::vector<int> execution_order;

    // Submit a long-running low-priority task first to occupy the worker.
    [[maybe_unused]] auto f0 = pool.submit(Task([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }, "first", TaskPriority::Low));

    // Let the low-priority task start executing.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Now submit higher-priority tasks.  They should execute first
    // when the worker finishes the current task.
    auto f1 = pool.submit(Task([&execution_order] {
        execution_order.push_back(1);
    }, "high", TaskPriority::High));

    auto f2 = pool.submit(Task([&execution_order] {
        execution_order.push_back(2);
    }, "normal", TaskPriority::Normal));

    f1.get();
    f2.get();

    EXPECT_EQ(execution_order.size(), 2u);
    EXPECT_EQ(execution_order[0], 1);
    EXPECT_EQ(execution_order[1], 2);
}

// ==========================================================================
// Group 12: Combined patterns
// ==========================================================================

TEST(ThreadPoolCombined, FanOutFanIn) {
    ThreadPool pool(make_config(4));
    constexpr int N = 100;

    std::vector<std::future<int>> futures;
    futures.reserve(N);
    for (int i = 0; i < N; ++i) {
        futures.push_back(pool.submit([](int x) { return x * x; }, i));
    }

    int total = 0;
    for (auto& f : futures) {
        total += f.get();
    }

    // sum of squares 0..99 = 328350
    EXPECT_EQ(total, 328350);
}

TEST(ThreadPoolCombined, ChainedFutures) {
    ThreadPool pool(make_config(2));

    // Stage 1: compute a value.
    auto f1 = pool.submit([] { return 10; });

    // Stage 2: use the result of stage 1.
    // We need to wait for f1, then submit the next task.
    int intermediate = f1.get();
    auto f2 = pool.submit([](int x) { return x * 2; }, intermediate);

    EXPECT_EQ(f2.get(), 20);
}

TEST(ThreadPoolCombined, AllReturnTypesInOnePool) {
    ThreadPool pool(make_config(4));

    auto f1 = pool.submit([] { return 42; });
    auto f2 = pool.submit([] { return std::string("hi"); });
    auto f3 = pool.submit([] { return Point{1, 2}; });
    auto f4 = pool.submit([] { return 3.14; });
    auto f5 = pool.submit([]() -> std::vector<int> { return {1, 2, 3}; });

    EXPECT_EQ(f1.get(), 42);
    EXPECT_EQ(f2.get(), "hi");
    EXPECT_EQ(f3.get().manhattan(), 3);
    EXPECT_DOUBLE_EQ(f4.get(), 3.14);
    EXPECT_EQ(f5.get().size(), 3u);
}

TEST(ThreadPoolCombined, PackagedTaskAndLambda) {
    ThreadPool pool(make_config(2));

    // Mix packaged_task and lambda submits on the same pool.
    std::packaged_task<int()> pt([] { return 100; });
    auto f1 = pool.submit(std::move(pt));

    auto f2 = pool.submit([] { return 200; });

    EXPECT_EQ(f1.get(), 100);
    EXPECT_EQ(f2.get(), 200);
}

// ==========================================================================
// Auto-scaling tests
// ==========================================================================

namespace {

ThreadPoolConfig make_scaling_config(std::size_t min_t, std::size_t max_t,
                                      std::size_t threshold = 3) {
    ThreadPoolConfig c;
    c.min_threads = min_t;
    c.max_threads = max_t;
    c.max_queue_size = 1000;
    c.scale_up_threshold = threshold;
    return c;
}

}  // namespace

TEST(ThreadPoolAutoScale, ScaleUpOnQueuePressure) {
    auto config = make_scaling_config(2, 8, /*threshold=*/3);
    ThreadPool pool(config);
    EXPECT_EQ(pool.thread_count(), 2u);

    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};

    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::vector<std::future<void>> futures;

    for (int i = 0; i < 20; ++i) {
        futures.push_back(pool.submit([&]() {
            gate_future.wait();
            int cur = running.fetch_add(1, std::memory_order_relaxed) + 1;
            int prev = max_concurrent.load(std::memory_order_relaxed);
            while (cur > prev &&
                   !max_concurrent.compare_exchange_weak(prev, cur,
                       std::memory_order_relaxed)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            running.fetch_sub(1, std::memory_order_relaxed);
        }));
    }

    gate.set_value();
    for (auto& f : futures) f.get();

    EXPECT_GE(pool.thread_count(), 2u);
    EXPECT_LE(pool.thread_count(), 8u);
}

TEST(ThreadPoolAutoScale, ScaleDownAfterIdle) {
    auto config = make_scaling_config(2, 6, /*threshold=*/2);
    ThreadPool pool(config);

    {
        std::vector<std::future<void>> futures;
        for (int i = 0; i < 20; ++i) {
            futures.push_back(pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }));
        }
        for (auto& f : futures) f.get();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < 30; ++i) {
        pool.submit([]() {}).get();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(pool.thread_count(), 2u);
}

TEST(ThreadPoolAutoScale, RespectsMinThreads) {
    auto config = make_scaling_config(4, 8, /*threshold=*/2);
    ThreadPool pool(config);
    EXPECT_EQ(pool.thread_count(), 4u);

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 20; ++i) {
        futures.push_back(pool.submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }));
    }
    for (auto& f : futures) f.get();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < 10; ++i) {
        pool.submit([]() {}).get();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_GE(pool.thread_count(), 4u);
}

TEST(ThreadPoolAutoScale, RespectsMaxThreads) {
    auto config = make_scaling_config(2, 4, /*threshold=*/2);
    ThreadPool pool(config);

    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::atomic<int> active{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 30; ++i) {
        futures.push_back(pool.submit([&]() {
            gate_future.wait();
            active.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            active.fetch_sub(1, std::memory_order_relaxed);
        }));
    }

    gate.set_value();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_LE(pool.thread_count(), 4u);

    for (auto& f : futures) f.get();
}

TEST(ThreadPoolAutoScale, AllTasksCompleteAfterScaleUp) {
    auto config = make_scaling_config(2, 8, /*threshold=*/2);
    ThreadPool pool(config);

    constexpr int N = 50;
    std::vector<std::future<int>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i) {
        futures.push_back(pool.submit([i] { return i * i; }));
    }

    int total = 0;
    for (auto& f : futures) {
        total += f.get();
    }

    int expected = 0;
    for (int i = 0; i < N; ++i) expected += i * i;
    EXPECT_EQ(total, expected);
}

TEST(ThreadPoolAutoScale, ConcurrentScaleUpDown) {
    auto config = make_scaling_config(2, 8, /*threshold=*/3);
    ThreadPool pool(config);

    for (int round = 0; round < 5; ++round) {
        std::promise<void> gate;
        auto gate_future = gate.get_future();
        std::vector<std::future<void>> futures;

        for (int i = 0; i < 15; ++i) {
            futures.push_back(pool.submit([&]() {
                gate_future.wait();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }));
        }
        gate.set_value();
        for (auto& f : futures) f.get();

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    EXPECT_GE(pool.thread_count(), 2u);
    EXPECT_LE(pool.thread_count(), 8u);
}

TEST(ThreadPoolAutoScale, ScaleDownThenBackUp) {
    auto config = make_scaling_config(2, 8, /*threshold=*/2);
    ThreadPool pool(config);

    {
        std::vector<std::future<void>> futures;
        for (int i = 0; i < 20; ++i) {
            futures.push_back(pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }));
        }
        for (auto& f : futures) f.get();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < 20; ++i) {
        pool.submit([]() {}).get();
    }

    EXPECT_GE(pool.thread_count(), 2u);
}

TEST(ThreadPoolAutoScale, StressConcurrentSubmits) {
    auto config = make_scaling_config(2, 8, /*threshold=*/3);
    ThreadPool pool(config);

    constexpr int NUM_THREADS = 8;
    constexpr int TASKS_PER_THREAD = 25;
    std::atomic<int> completed{0};
    std::vector<std::thread> submitters;

    for (int t = 0; t < NUM_THREADS; ++t) {
        submitters.emplace_back([&]() {
            for (int i = 0; i < TASKS_PER_THREAD; ++i) {
                pool.submit([&]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    completed.fetch_add(1, std::memory_order_relaxed);
                }).get();
            }
        });
    }

    for (auto& t : submitters) t.join();

    EXPECT_EQ(completed.load(), NUM_THREADS * TASKS_PER_THREAD);
}

TEST(ThreadPoolAutoScale, StressBurstThenDrain) {
    auto config = make_scaling_config(2, 8, /*threshold=*/2);
    ThreadPool pool(config);

    for (int burst = 0; burst < 10; ++burst) {
        std::vector<std::future<int>> futures;
        for (int i = 0; i < 30; ++i) {
            futures.push_back(pool.submit([i] { return i; }));
        }
        int burst_sum = 0;
        for (auto& f : futures) {
            burst_sum += f.get();
        }
        int expected = 0;
        for (int i = 0; i < 30; ++i) expected += i;
        EXPECT_EQ(burst_sum, expected);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

TEST(ThreadPoolAutoScale, ScaleUpAndCorrectness) {
    auto config = make_scaling_config(2, 8, /*threshold=*/2);
    ThreadPool pool(config);

    std::atomic<int> max_seen{0};
    std::atomic<int> active{0};
    std::promise<void> gate;
    auto gate_future = gate.get_future();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 40; ++i) {
        futures.push_back(pool.submit([&]() {
            gate_future.wait();
            int cur = active.fetch_add(1, std::memory_order_relaxed) + 1;
            int prev = max_seen.load(std::memory_order_relaxed);
            while (cur > prev &&
                   !max_seen.compare_exchange_weak(prev, cur,
                       std::memory_order_relaxed)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            active.fetch_sub(1, std::memory_order_relaxed);
        }));
    }

    gate.set_value();
    for (auto& f : futures) f.get();

    EXPECT_GT(max_seen.load(), 2);
}

TEST(ThreadPoolAutoScale, ShutdownDuringScaleDown) {
    auto config = make_scaling_config(2, 8, /*threshold=*/2);

    for (int iter = 0; iter < 5; ++iter) {
        ThreadPool pool(config);
        for (int i = 0; i < 10; ++i) {
            std::ignore = pool.submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            });
        }
    }
}

TEST(ThreadPoolAutoScale, NoScaleWhenBelowThreshold) {
    auto config = make_scaling_config(2, 8, /*threshold=*/10);
    ThreadPool pool(config);

    for (int i = 0; i < 5; ++i) {
        pool.submit([]() {}).get();
    }

    EXPECT_EQ(pool.thread_count(), 2u);
}

}  // namespace
}  // namespace ctee
