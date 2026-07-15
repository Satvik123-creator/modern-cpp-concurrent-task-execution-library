#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ctee/core/task_priority.h"
#include "ctee/core/task_result.h"

namespace ctee {

struct TaskInfo {
    std::string id;
    std::string name;
    std::string task_type;
    TaskPriority priority = TaskPriority::Normal;
    TaskStatus status = TaskStatus::Pending;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::milliseconds execution_time{0};
    std::string error_message;
};

class TaskRegistry {
public:
    TaskRegistry() = default;

    TaskRegistry(const TaskRegistry&) = delete;
    TaskRegistry& operator=(const TaskRegistry&) = delete;

    void register_task(const std::string& id, TaskInfo info);

    void update_status(const std::string& id, TaskStatus status);
    void update_status(const std::string& id, TaskStatus status,
                       std::chrono::milliseconds execution_time);
    bool update_status_if_not_terminal(const std::string& id, TaskStatus status);
    void update_error(const std::string& id, const std::string& message);

    [[nodiscard]] std::optional<TaskInfo> get(const std::string& id) const;
    [[nodiscard]] std::vector<TaskInfo> list() const;
    [[nodiscard]] std::vector<TaskInfo> list(TaskStatus filter) const;
    [[nodiscard]] std::size_t count() const;
    [[nodiscard]] std::size_t count(TaskStatus status) const;

    void set_cancel_handler(const std::string& id,
                            std::function<bool()> handler);
    bool cancel(const std::string& id);

    void remove(const std::string& id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TaskInfo> tasks_;
    std::unordered_map<std::string, std::function<bool()>> cancel_handlers_;
};

}  // namespace ctee
