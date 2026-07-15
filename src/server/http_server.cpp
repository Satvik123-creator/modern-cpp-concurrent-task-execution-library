#include "ctee/server/http_server.h"
#include "ctee/common/logger.h"

#include <httplib.h>

namespace ctee {

HTTPServer::HTTPServer(ThreadPool& pool, Scheduler& scheduler, std::uint16_t port)
    : pool_(pool), scheduler_(scheduler), port_(port),
      handler_(pool, scheduler) {
    server_ = std::make_unique<httplib::Server>();
    server_->set_idle_interval(std::chrono::milliseconds(100));
}

HTTPServer::~HTTPServer() {
    stop();
}

void HTTPServer::start() {
    setup_routes();
    if (!server_->bind_to_port("0.0.0.0", port_)) {
        Logger::instance().error("Failed to bind to port " + std::to_string(port_));
        return;
    }
    running_ = true;
    Logger::instance().info("HTTP Server starting on port " + std::to_string(port_));
    server_thread_ = std::thread([this]() {
        server_->listen_after_bind();
    });
}

void HTTPServer::stop() {
    if (running_.exchange(false)) {
        server_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
}

bool HTTPServer::is_running() const noexcept {
    return running_;
}

std::uint16_t HTTPServer::port() const noexcept {
    return port_;
}

void HTTPServer::setup_routes() {
    auto& h = handler_;

    server_->Post("/tasks", [&h](const httplib::Request& req, httplib::Response& res) {
        h.submit_task(req, res);
    });

    server_->Post("/tasks/priority", [&h](const httplib::Request& req, httplib::Response& res) {
        h.submit_task_priority(req, res);
    });

    server_->Post("/tasks/schedule", [&h](const httplib::Request& req, httplib::Response& res) {
        h.schedule_task(req, res);
    });

    server_->Delete(R"(/tasks/([^/]+))", [&h](const httplib::Request& req, httplib::Response& res) {
        h.cancel_task(req, res);
    });

    server_->Get("/workers", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_workers(req, res);
    });

    server_->Get("/queue", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_queue(req, res);
    });

    server_->Get("/metrics", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_metrics(req, res);
    });

    server_->Get("/health", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_health(req, res);
    });

    server_->Post("/tasks/batch", [&h](const httplib::Request& req, httplib::Response& res) {
        h.submit_batch(req, res);
    });

    server_->Get("/tasks/history", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_task_history(req, res);
    });

    server_->Get("/reports/performance", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_performance_report(req, res);
    });

    server_->Post("/pool/pause", [&h](const httplib::Request& req, httplib::Response& res) {
        h.pause_pool(req, res);
    });

    server_->Post("/pool/resume", [&h](const httplib::Request& req, httplib::Response& res) {
        h.resume_pool(req, res);
    });

    server_->Get("/config", [&h](const httplib::Request& req, httplib::Response& res) {
        h.get_config(req, res);
    });

    server_->Post("/shutdown", [&h](const httplib::Request& req, httplib::Response& res) {
        h.shutdown_pool(req, res);
    });

    server_->Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    server_->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });
}

}  // namespace ctee
