#include "ctee/core/task_history.h"

namespace ctee {

TaskHistory::TaskHistory(std::size_t max_size) : max_size_(max_size) {}

void TaskHistory::record(const TaskRecord& entry) {
    std::lock_guard lock(mutex_);
    entries_.push_back(entry);
    while (entries_.size() > max_size_) {
        entries_.pop_front();
    }
}

std::vector<TaskRecord> TaskHistory::recent(std::size_t count) const {
    std::lock_guard lock(mutex_);
    std::vector<TaskRecord> result;
    auto start = entries_.size() > count ? entries_.size() - count : 0;
    result.reserve(entries_.size() - start);
    for (auto i = start; i < entries_.size(); ++i) {
        result.push_back(entries_[i]);
    }
    return result;
}

std::vector<TaskRecord> TaskHistory::since(std::chrono::steady_clock::time_point since) const {
    std::lock_guard lock(mutex_);
    std::vector<TaskRecord> result;
    for (const auto& entry : entries_) {
        if (entry.completed_at >= since) {
            result.push_back(entry);
        }
    }
    return result;
}

std::size_t TaskHistory::count() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void TaskHistory::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

void TaskHistory::record_submit(const std::string& task_id,
                                const std::string& task_name,
                                std::chrono::steady_clock::time_point at) {
    TaskRecord entry;
    entry.task_id = task_id;
    entry.task_name = task_name;
    entry.status = TaskStatus::Queued;
    entry.submitted_at = at;
    record(entry);
}

void TaskHistory::record_complete(const std::string& task_id,
                                  TaskStatus status,
                                  std::chrono::milliseconds exec_time) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_) {
        if (e.task_id == task_id) {
            e.status = status;
            e.completed_at = std::chrono::steady_clock::now();
            e.execution_time = exec_time;
            return;
        }
    }
}

}  // namespace ctee
