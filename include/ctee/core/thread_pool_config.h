#pragma once

#include <cstddef>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace ctee {

struct ThreadPoolConfig {
    std::size_t min_threads = 2;
    std::size_t max_threads = std::max<std::size_t>(2, std::thread::hardware_concurrency());
    std::size_t max_queue_size = 10000;
    std::chrono::milliseconds task_timeout{30000};
    bool enable_metrics = true;

    // Auto-scaling parameters
    std::chrono::milliseconds idle_timeout{5000};
    std::size_t scale_up_threshold = 10;

    // Retry / timeout defaults applied when tasks don't override
    std::size_t default_max_retries = 0;
    std::chrono::milliseconds default_retry_delay{1000};
    std::chrono::milliseconds default_task_timeout{0}; // 0 = no timeout
};

struct ThreadPoolStats {
    std::size_t active_threads = 0;
    std::size_t pending_tasks = 0;
    std::size_t completed_tasks = 0;
    std::size_t failed_tasks = 0;
    std::size_t total_submitted = 0;
    std::size_t total_cancelled = 0;
    std::size_t running_tasks = 0;
    std::size_t peak_queue_length = 0;
    double avg_wait_time_ms = 0.0;
    double avg_exec_time_ms = 0.0;
    double worker_utilization = 0.0;
    double tasks_per_sec = 0.0;
    double uptime_seconds = 0.0;
};

struct WorkerInfo {
    std::size_t id = 0;
    std::string state;
    bool marked_stop = false;
};

struct QueueInfo {
    std::size_t size = 0;
    std::size_t capacity = 0;
    bool is_full = false;
};

}  // namespace ctee
