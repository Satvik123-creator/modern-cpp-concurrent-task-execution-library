#include "ctee/server/task_registry.h"

namespace ctee {

void TaskRegistry::register_task(const std::string& id, TaskInfo info) {
    std::lock_guard lock(mutex_);
    tasks_.emplace(id, std::move(info));
}

void TaskRegistry::update_status(const std::string& id, TaskStatus status) {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
        it->second.status = status;
    }
}

void TaskRegistry::update_status(const std::string& id, TaskStatus status,
                                  std::chrono::milliseconds execution_time) {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
        it->second.status = status;
        it->second.execution_time = execution_time;
    }
}

bool TaskRegistry::update_status_if_not_terminal(const std::string& id,
                                                  TaskStatus status) {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return false;
    auto current = it->second.status;
    if (current == TaskStatus::Completed ||
        current == TaskStatus::Failed ||
        current == TaskStatus::Cancelled) {
        return false;
    }
    it->second.status = status;
    return true;
}

void TaskRegistry::update_error(const std::string& id,
                                 const std::string& message) {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
        it->second.error_message = message;
    }
}

std::optional<TaskInfo> TaskRegistry::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<TaskInfo> TaskRegistry::list() const {
    std::lock_guard lock(mutex_);
    std::vector<TaskInfo> result;
    result.reserve(tasks_.size());
    for (const auto& [id, info] : tasks_) {
        result.push_back(info);
    }
    return result;
}

std::vector<TaskInfo> TaskRegistry::list(TaskStatus filter) const {
    std::lock_guard lock(mutex_);
    std::vector<TaskInfo> result;
    for (const auto& [id, info] : tasks_) {
        if (info.status == filter) {
            result.push_back(info);
        }
    }
    return result;
}

std::size_t TaskRegistry::count() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

std::size_t TaskRegistry::count(TaskStatus status) const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [id, info] : tasks_) {
        if (info.status == status) ++n;
    }
    return n;
}

void TaskRegistry::set_cancel_handler(const std::string& id,
                                       std::function<bool()> handler) {
    std::lock_guard lock(mutex_);
    cancel_handlers_[id] = std::move(handler);
}

bool TaskRegistry::cancel(const std::string& id) {
    std::function<bool()> handler;
    {
        std::lock_guard lock(mutex_);
        auto it = cancel_handlers_.find(id);
        if (it == cancel_handlers_.end()) return false;
        handler = it->second;
        cancel_handlers_.erase(it);
    }
    bool result = handler();
    if (result) {
        update_status(id, TaskStatus::Cancelled);
    }
    return result;
}

void TaskRegistry::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    tasks_.erase(id);
    cancel_handlers_.erase(id);
}

}  // namespace ctee
