#include "ctee/common/config.h"
#include "ctee/common/logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ctee {

AppConfig load_config(const std::string& path) {
    AppConfig config;
    
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::instance().info("No config file found at " + path + ", using defaults");
        return config;
    }
    
    try {
        json j;
        file >> j;
        
        if (j.contains("pool")) {
            auto& p = j["pool"];
            if (p.contains("min_threads")) config.pool.min_threads = p["min_threads"].get<std::size_t>();
            if (p.contains("max_threads")) config.pool.max_threads = p["max_threads"].get<std::size_t>();
            if (p.contains("max_queue_size")) config.pool.max_queue_size = p["max_queue_size"].get<std::size_t>();
            if (p.contains("idle_timeout_ms")) config.pool.idle_timeout = std::chrono::milliseconds(p["idle_timeout_ms"].get<long long>());
            if (p.contains("scale_up_threshold")) config.pool.scale_up_threshold = p["scale_up_threshold"].get<std::size_t>();
        }
        
        if (j.contains("server")) {
            auto& s = j["server"];
            if (s.contains("port")) config.server.port = s["port"].get<std::uint16_t>();
            if (s.contains("host")) config.server.host = s["host"].get<std::string>();
        }
        
        if (j.contains("retry")) {
            auto& r = j["retry"];
            if (r.contains("max_retries")) config.default_max_retries = r["max_retries"].get<std::size_t>();
            if (r.contains("delay_ms")) config.default_retry_delay = std::chrono::milliseconds(r["delay_ms"].get<long long>());
        }
        
        if (j.contains("timeout_ms")) {
            config.default_task_timeout = std::chrono::milliseconds(j["timeout_ms"].get<long long>());
        }
        
        if (j.contains("max_history_size")) {
            config.max_history_size = j["max_history_size"].get<std::size_t>();
        }
        
        if (j.contains("name_threads")) {
            config.name_threads = j["name_threads"].get<bool>();
        }
        
        Logger::instance().info("Config loaded from " + path);
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to parse config: " + std::string(e.what()));
    }
    
    return config;
}

}  // namespace ctee
