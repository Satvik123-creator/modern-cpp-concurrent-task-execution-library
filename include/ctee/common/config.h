#pragma once

#include <cstddef>
#include <chrono>
#include <string>

#include "ctee/core/thread_pool_config.h"
#include "ctee/server/server_config.h"

namespace ctee {

struct AppConfig {
    ThreadPoolConfig pool;
    ServerConfig server;
    
    // Retry defaults
    std::size_t default_max_retries = 0;
    std::chrono::milliseconds default_retry_delay{1000};
    
    // Timeout default
    std::chrono::milliseconds default_task_timeout{0};
    
    // History
    std::size_t max_history_size = 1000;
    
    // Thread naming
    bool name_threads = true;
};

// Load config from JSON file. Returns default config if file not found.
AppConfig load_config(const std::string& path);

}  // namespace ctee
