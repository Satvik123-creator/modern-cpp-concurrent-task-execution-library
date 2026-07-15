#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ctee/core/task_priority.h"
#include "ctee/core/task_result.h"

namespace ctee {

class ThreadPool;
namespace detail {
    class Worker;
}

class Task {
    friend class detail::Worker;
    friend class ThreadPool;

public:
    using FuncType = std::function<void()>;

    Task(FuncType func, std::string name = "",
         TaskPriority priority = TaskPriority::Normal);
    ~Task() = default;

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task() = delete;

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] TaskPriority priority() const noexcept;
    [[nodiscard]] TaskStatus status() const noexcept;
    [[nodiscard]] const FuncType& func() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point created_at() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point scheduled_at() const noexcept;
    [[nodiscard]] bool has_schedule() const noexcept;
    [[nodiscard]] bool is_recurring() const noexcept;
    [[nodiscard]] std::chrono::milliseconds repeat_interval() const noexcept;
    [[nodiscard]] std::size_t repeat_count() const noexcept;
    [[nodiscard]] std::size_t max_repeats() const noexcept;
    [[nodiscard]] const TaskResult& result() const noexcept;
    [[nodiscard]] std::size_t retry_count() const noexcept;
    [[nodiscard]] std::size_t max_retries() const noexcept;
    [[nodiscard]] std::chrono::milliseconds retry_delay() const noexcept;
    [[nodiscard]] const std::vector<std::string>& depends_on() const noexcept;
    [[nodiscard]] std::chrono::milliseconds timeout_ms() const noexcept;

    void set_scheduled_at(std::chrono::steady_clock::time_point tp);
    void set_recurring(std::chrono::milliseconds interval, std::size_t max_repeats = 0);
    void set_max_retries(std::size_t n);
    void set_retry_delay(std::chrono::milliseconds delay);
    void set_depends_on(std::vector<std::string> deps);
    void set_timeout(std::chrono::milliseconds ms);
    void set_id(const std::string& id);

    bool operator<(const Task& other) const noexcept;

private:
    void set_status(TaskStatus status);
    void increment_repeat_count();
    void increment_retry_count();
    void set_result(TaskResult result);
    void execute();

private:
    std::string id_;
    std::string name_;
    FuncType func_;
    TaskPriority priority_;
    TaskStatus status_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point scheduled_at_;
    bool has_schedule_;
    std::chrono::milliseconds repeat_interval_;
    std::size_t repeat_count_;
    std::size_t max_repeats_;
    TaskResult result_;
    std::size_t retry_count_ = 0;
    std::size_t max_retries_ = 0;
    std::chrono::milliseconds retry_delay_{0};
    std::vector<std::string> depends_on_;
    std::chrono::milliseconds timeout_ms_{0}; // 0 = no timeout
};

}  // namespace ctee
