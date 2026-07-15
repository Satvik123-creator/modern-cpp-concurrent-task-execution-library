#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "ctee/common/config.h"

namespace {

std::string temp_json_path() {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    return (dir / ("ctee_test_" + std::to_string(counter++) + ".json")).string();
}

}  // namespace

TEST(ConfigTest, LoadValidConfig) {
    auto path = temp_json_path();
    {
        std::ofstream f(path);
        f << R"({
            "pool": {"min_threads": 4, "max_threads": 8, "scale_up_threshold": 5},
            "server": {"port": 9090, "host": "127.0.0.1"},
            "retry": {"max_retries": 3, "delay_ms": 500},
            "timeout_ms": 10000,
            "max_history_size": 500,
            "name_threads": false
        })";
    }

    auto config = ctee::load_config(path);
    std::remove(path.c_str());

    EXPECT_EQ(config.pool.min_threads, 4u);
    EXPECT_EQ(config.pool.max_threads, 8u);
    EXPECT_EQ(config.pool.scale_up_threshold, 5u);
    EXPECT_EQ(config.server.port, 9090u);
    EXPECT_EQ(config.server.host, "127.0.0.1");
    EXPECT_EQ(config.default_max_retries, 3u);
    EXPECT_EQ(config.default_retry_delay.count(), 500);
    EXPECT_EQ(config.default_task_timeout.count(), 10000);
    EXPECT_EQ(config.max_history_size, 500u);
    EXPECT_FALSE(config.name_threads);
}

TEST(ConfigTest, LoadMissingFileReturnsDefaults) {
    auto config = ctee::load_config("/nonexistent/path/config.json");
    EXPECT_EQ(config.pool.min_threads, 2u);
    EXPECT_EQ(config.server.port, 8080u);
    EXPECT_EQ(config.default_max_retries, 0u);
}

TEST(ConfigTest, LoadMalformedJsonReturnsDefaults) {
    auto path = temp_json_path();
    {
        std::ofstream f(path);
        f << "this is not json {{{";
    }

    auto config = ctee::load_config(path);
    std::remove(path.c_str());

    EXPECT_EQ(config.pool.min_threads, 2u);
    EXPECT_EQ(config.server.port, 8080u);
}

TEST(ConfigTest, LoadEmptyJsonObjectReturnsDefaults) {
    auto path = temp_json_path();
    {
        std::ofstream f(path);
        f << "{}";
    }

    auto config = ctee::load_config(path);
    std::remove(path.c_str());

    EXPECT_EQ(config.pool.min_threads, 2u);
    EXPECT_EQ(config.pool.max_threads,
              std::max<std::size_t>(2, std::thread::hardware_concurrency()));
    EXPECT_EQ(config.pool.max_queue_size, 10000u);
    EXPECT_EQ(config.server.port, 8080u);
    EXPECT_EQ(config.server.host, "0.0.0.0");
    EXPECT_EQ(config.default_max_retries, 0u);
    EXPECT_EQ(config.default_retry_delay.count(), 1000);
    EXPECT_EQ(config.default_task_timeout.count(), 0);
    EXPECT_EQ(config.max_history_size, 1000u);
    EXPECT_TRUE(config.name_threads);
}

TEST(ConfigTest, LoadPartialJsonObjectAppliesDefaults) {
    auto path = temp_json_path();
    {
        std::ofstream f(path);
        f << R"({"pool": {"min_threads": 4}})";
    }

    auto config = ctee::load_config(path);
    std::remove(path.c_str());

    EXPECT_EQ(config.pool.min_threads, 4u);
    EXPECT_EQ(config.pool.max_queue_size, 10000u);
    EXPECT_EQ(config.server.port, 8080u);
    EXPECT_EQ(config.default_max_retries, 0u);
    EXPECT_EQ(config.max_history_size, 1000u);
}
