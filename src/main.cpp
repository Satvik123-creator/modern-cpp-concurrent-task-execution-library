#include <iostream>

#include "ctee/core/thread_pool.h"
#include "ctee/core/scheduler.h"
#include "ctee/server/http_server.h"
#include "ctee/common/logger.h"

int main() {
    ctee::Logger::instance().info("Concurrent Task Execution Engine starting...");

    ctee::ThreadPoolConfig config;
    config.min_threads = std::thread::hardware_concurrency();
    config.max_queue_size = 10000;

    ctee::ThreadPool pool(config);
    ctee::Scheduler scheduler(pool);
    ctee::HTTPServer server(pool, scheduler, 8080);

    server.start();

    ctee::Logger::instance().info("Server running on http://0.0.0.0:8080");
    ctee::Logger::instance().info("Press Enter to stop...");

    std::cin.get();

    ctee::Logger::instance().info("Shutting down...");
    server.stop();
    scheduler.shutdown();
    pool.shutdown();

    ctee::Logger::instance().info("Shutdown complete.");

    return 0;
}
