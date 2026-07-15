#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ctee/core/thread_pool.h"
#include "ctee/core/scheduler.h"
#include "ctee/server/http_server.h"

using json = nlohmann::json;

namespace ctee {
namespace {

std::atomic<std::uint16_t> g_port_counter{18200};

class HTTPAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = g_port_counter.fetch_add(1);

        ThreadPoolConfig config;
        config.min_threads = 2;
        config.max_threads = 4;
        pool_ = std::make_unique<ThreadPool>(config);
        scheduler_ = std::make_unique<Scheduler>(*pool_);
        server_ = std::make_unique<HTTPServer>(*pool_, *scheduler_, port_);
        server_->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        scheduler_->shutdown();
        pool_->shutdown();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<HTTPServer> server_;
    std::uint16_t port_;
};

// ==========================================================================
// GET /health
// ==========================================================================

TEST_F(HTTPAPITest, HealthEndpoint) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Get("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_TRUE(body.contains("pool_running"));
    EXPECT_TRUE(body.contains("scheduler_running"));
}

// ==========================================================================
// GET /metrics
// ==========================================================================

TEST_F(HTTPAPITest, MetricsEndpoint) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Get("/metrics");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("active_threads"));
    EXPECT_TRUE(body.contains("queue_length"));
    EXPECT_TRUE(body.contains("completed_tasks"));
    EXPECT_TRUE(body.contains("failed_tasks"));
    EXPECT_TRUE(body.contains("running_tasks"));
    EXPECT_TRUE(body.contains("peak_queue_length"));
    EXPECT_TRUE(body.contains("avg_wait_time_ms"));
    EXPECT_TRUE(body.contains("avg_exec_time_ms"));
    EXPECT_TRUE(body.contains("worker_utilization"));
    EXPECT_TRUE(body.contains("tasks_per_sec"));
    EXPECT_TRUE(body.contains("is_running"));
    EXPECT_TRUE(body.contains("history"));
}

// ==========================================================================
// POST /tasks
// ==========================================================================

TEST_F(HTTPAPITest, SubmitNoopTask) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";
    req_body["name"] = "test_noop";

    auto res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("task_id"));
    EXPECT_EQ(body["name"], "test_noop");
    EXPECT_EQ(body["task_type"], "noop");
    EXPECT_EQ(body["status"], "queued");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(HTTPAPITest, SubmitComputeTask) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "compute";
    req_body["name"] = "test_compute";
    req_body["iterations"] = 1000;

    auto res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("task_id"));
    EXPECT_EQ(body["task_type"], "compute");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(HTTPAPITest, SubmitSleepTask) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "sleep";
    req_body["duration_ms"] = 50;

    auto res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("task_id"));
}

TEST_F(HTTPAPITest, SubmitTaskInvalidJSON) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/tasks", "not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("error"));
}

TEST_F(HTTPAPITest, SubmitTaskMissingType) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["name"] = "no_type";

    auto res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("error"));
}

TEST_F(HTTPAPITest, SubmitTaskUnknownType) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "bogus";

    auto res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("error"));
}

TEST_F(HTTPAPITest, SubmitTaskInvalidPriority) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";
    req_body["priority"] = "urgent";

    auto res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, SubmitWithPriority) {
    httplib::Client cli("localhost", port_);

    for (const auto& p : {"low", "normal", "high", "critical"}) {
        json req_body;
        req_body["task_type"] = "noop";
        req_body["priority"] = p;
        auto res = cli.Post("/tasks", req_body.dump(), "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 201);
    }
}

// ==========================================================================
// POST /tasks/priority
// ==========================================================================

TEST_F(HTTPAPITest, SubmitPriorityTask) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";
    req_body["priority"] = "high";
    req_body["name"] = "priority_task";

    auto res = cli.Post("/tasks/priority", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("task_id"));
    EXPECT_EQ(body["priority"], "high");
    EXPECT_EQ(body["name"], "priority_task");
}

TEST_F(HTTPAPITest, SubmitPriorityTaskAllLevels) {
    httplib::Client cli("localhost", port_);

    for (const auto& p : {"low", "normal", "high", "critical"}) {
        json req_body;
        req_body["task_type"] = "noop";
        req_body["priority"] = p;
        auto res = cli.Post("/tasks/priority", req_body.dump(), "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 201);
        auto body = json::parse(res->body);
        EXPECT_EQ(body["priority"], p);
    }
}

TEST_F(HTTPAPITest, SubmitPriorityTaskMissingPriority) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";

    auto res = cli.Post("/tasks/priority", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("error"));
}

TEST_F(HTTPAPITest, SubmitPriorityTaskInvalidJSON) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/tasks/priority", "bad json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, SubmitPriorityTaskInvalidPriority) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";
    req_body["priority"] = "urgent";

    auto res = cli.Post("/tasks/priority", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ==========================================================================
// POST /tasks/schedule
// ==========================================================================

TEST_F(HTTPAPITest, ScheduleTask) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";
    req_body["delay_ms"] = 100;
    req_body["name"] = "scheduled_noop";

    auto res = cli.Post("/tasks/schedule", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("task_id"));
    EXPECT_EQ(body["delay_ms"], 100);
    EXPECT_EQ(body["status"], "scheduled");
}

TEST_F(HTTPAPITest, ScheduleTaskInvalidJSON) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/tasks/schedule", "not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, ScheduleTaskMissingFields) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";

    auto res = cli.Post("/tasks/schedule", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, ScheduleTaskNegativeDelay) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";
    req_body["delay_ms"] = -100;

    auto res = cli.Post("/tasks/schedule", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ==========================================================================
// DELETE /tasks/{id}
// ==========================================================================

TEST_F(HTTPAPITest, GetTaskStatus) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";

    auto submit_res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(submit_res);
    auto submit_body = json::parse(submit_res->body);
    std::string task_id = submit_body["task_id"];

    std::string url = "/tasks/" + task_id;
    auto res = cli.Delete(url.c_str());
    ASSERT_TRUE(res);

    auto body = json::parse(res->body);
    if (res->status == 200) {
        EXPECT_TRUE(body.contains("cancelled"));
        EXPECT_TRUE(body["cancelled"].get<bool>());
    } else {
        EXPECT_EQ(res->status, 409);
    }
}

TEST_F(HTTPAPITest, CancelCompletedTask) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";

    auto submit_res = cli.Post("/tasks", req_body.dump(), "application/json");
    ASSERT_TRUE(submit_res);
    auto submit_body = json::parse(submit_res->body);
    std::string task_id = submit_body["task_id"];

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string url = "/tasks/" + task_id;
    auto res = cli.Delete(url.c_str());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409);
}

TEST_F(HTTPAPITest, CancelNotFound) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Delete("/tasks/nonexistent-id");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// ==========================================================================
// GET /workers
// ==========================================================================

TEST_F(HTTPAPITest, WorkersEndpoint) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Get("/workers");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("workers"));
    EXPECT_TRUE(body.contains("total"));
    EXPECT_GE(body["total"].get<std::size_t>(), 2u);

    auto& workers = body["workers"];
    ASSERT_TRUE(workers.is_array());
    EXPECT_GE(workers.size(), 2u);

    auto& first = workers[0];
    EXPECT_TRUE(first.contains("id"));
    EXPECT_TRUE(first.contains("state"));
    EXPECT_TRUE(first.contains("marked_stop"));
}

TEST_F(HTTPAPITest, WorkersStateValues) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Get("/workers");
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);

    for (const auto& w : body["workers"]) {
        std::string state = w["state"].get<std::string>();
        EXPECT_TRUE(state == "idle" || state == "waiting" || state == "running" || state == "stopped");
    }
}

// ==========================================================================
// GET /queue
// ==========================================================================

TEST_F(HTTPAPITest, QueueEndpoint) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Get("/queue");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("size"));
    EXPECT_TRUE(body.contains("capacity"));
    EXPECT_TRUE(body.contains("is_full"));
    EXPECT_GE(body["capacity"].get<std::size_t>(), 1u);
}

TEST_F(HTTPAPITest, QueueEmpty) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Get("/queue");
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);

    // Pool is idle, queue should be empty
    EXPECT_EQ(body["size"].get<std::size_t>(), 0u);
    EXPECT_FALSE(body["is_full"].get<bool>());
}

// ==========================================================================
// POST /shutdown
// ==========================================================================

TEST_F(HTTPAPITest, ShutdownEndpoint) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/shutdown");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "shutting_down");
}

// ==========================================================================
// Validation / error handling
// ==========================================================================

TEST_F(HTTPAPITest, SubmitEmptyBody) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/tasks", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, SubmitPriorityEmptyBody) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/tasks/priority", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, ScheduleEmptyBody) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Post("/tasks/schedule", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, ScheduleMissingDelay) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["task_type"] = "noop";

    auto res = cli.Post("/tasks/schedule", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(HTTPAPITest, ScheduleMissingTaskType) {
    httplib::Client cli("localhost", port_);
    json req_body;
    req_body["delay_ms"] = 100;

    auto res = cli.Post("/tasks/schedule", req_body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ==========================================================================
// CORS
// ==========================================================================

TEST_F(HTTPAPITest, OptionsReturns204) {
    httplib::Client cli("localhost", port_);
    auto res = cli.Options("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
}

// ==========================================================================
// Integration: submit + metrics
// ==========================================================================

TEST_F(HTTPAPITest, StatsReflectSubmissions) {
    httplib::Client cli("localhost", port_);

    json req_body;
    req_body["task_type"] = "noop";
    cli.Post("/tasks", req_body.dump(), "application/json");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto res = cli.Get("/metrics");
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);
    EXPECT_GE(body["completed_tasks"].get<std::size_t>(), 1u);
}

TEST_F(HTTPAPITest, SubmitMultipleTasksAndCheckMetrics) {
    httplib::Client cli("localhost", port_);

    for (int i = 0; i < 5; ++i) {
        json req_body;
        req_body["task_type"] = "compute";
        req_body["iterations"] = 100;
        req_body["name"] = "batch_" + std::to_string(i);
        cli.Post("/tasks", req_body.dump(), "application/json");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto res = cli.Get("/metrics");
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);
    EXPECT_GE(body["completed_tasks"].get<std::size_t>(), 5u);
}

}  // namespace
}  // namespace ctee
