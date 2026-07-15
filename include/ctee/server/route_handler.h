#pragma once

#include <memory>
#include <string>

#include "ctee/core/metrics.h"
#include "ctee/core/thread_pool.h"
#include "ctee/core/scheduler.h"
#include "ctee/server/task_registry.h"

namespace httplib {
struct Request;
struct Response;
}

namespace ctee {

class RouteHandler {
public:
    RouteHandler(ThreadPool& pool, Scheduler& scheduler);

    void submit_task(const httplib::Request& req, httplib::Response& res);
    void submit_task_priority(const httplib::Request& req, httplib::Response& res);
    void schedule_task(const httplib::Request& req, httplib::Response& res);
    void cancel_task(const httplib::Request& req, httplib::Response& res);

    void get_workers(const httplib::Request& req, httplib::Response& res);
    void get_queue(const httplib::Request& req, httplib::Response& res);
    void get_metrics(const httplib::Request& req, httplib::Response& res);
    void get_health(const httplib::Request& req, httplib::Response& res);

    void submit_batch(const httplib::Request& req, httplib::Response& res);
    void get_task_history(const httplib::Request& req, httplib::Response& res);
    void get_performance_report(const httplib::Request& req, httplib::Response& res);
    void pause_pool(const httplib::Request& req, httplib::Response& res);
    void resume_pool(const httplib::Request& req, httplib::Response& res);
    void get_config(const httplib::Request& req, httplib::Response& res);

    void shutdown_pool(const httplib::Request& req, httplib::Response& res);

private:
    ThreadPool& pool_;
    Scheduler& scheduler_;
    TaskRegistry registry_;
};

}  // namespace ctee
