#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "ctee/core/thread_pool.h"
#include "ctee/core/scheduler.h"
#include "ctee/server/route_handler.h"

namespace httplib {
class Server;
}

namespace ctee {

class HTTPServer {
public:
    HTTPServer(ThreadPool& pool, Scheduler& scheduler, std::uint16_t port = 8080);
    ~HTTPServer();

    HTTPServer(const HTTPServer&) = delete;
    HTTPServer& operator=(const HTTPServer&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    void setup_routes();

    ThreadPool& pool_;
    Scheduler& scheduler_;
    std::uint16_t port_;
    RouteHandler handler_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ctee
