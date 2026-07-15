// basic_pool.cpp — Demonstrates basic thread pool usage.
//
// Build (after cmake --build build):
//   g++ -std=c++20 -I../include basic_pool.cpp -L../build/src -lctee_core -lpthread -o basic_pool

#include <iostream>
#include <future>
#include <vector>

#include "ctee/core/thread_pool.h"

int main() {
    // 1. Create a pool with 4 threads
    ctee::ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.max_queue_size = 1000;

    ctee::ThreadPool pool(config);

    // 2. Submit lambdas with automatic type deduction
    auto future_sum = pool.submit([]() { return 42; });
    std::cout << "Sum: " << future_sum.get() << "\n";

    // 3. Submit lambdas with arguments
    auto future_product = pool.submit([](int a, int b) { return a * b; }, 6, 7);
    std::cout << "Product: " << future_product.get() << "\n";

    // 4. Fan-out: submit many tasks, collect results
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([i]() { return i * i; }));
    }

    std::cout << "Squares: ";
    for (auto& f : futures) {
        std::cout << f.get() << " ";
    }
    std::cout << "\n";

    // 5. Submit a Task object with retry support
    ctee::Task task([]() {
        std::cout << "Task executed!\n";
    }, "my_task", ctee::TaskPriority::High);

    auto task_future = pool.submit(std::move(task));
    task_future.get();

    // 6. Check stats
    auto stats = pool.stats();
    std::cout << "Completed: " << stats.completed_tasks
              << ", Workers: " << stats.active_threads << "\n";

    pool.shutdown();
    return 0;
}
