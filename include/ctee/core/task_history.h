#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "ctee/core/task_result.h"

namespace ctee {

struct TaskRecord {
    std::string task_id;
    std::string task_name;
    TaskStatus status = TaskStatus::Pending;
    std::chrono::steady_clock::time_point submitted_at;
    std::chrono::steady_clock::time_point completed_at;
    std::chrono::milliseconds execution_time{0};
};

class TaskHistory {
public:
    explicit TaskHistory(std::size_t max_size = 10000);

    void record(const TaskRecord& entry);

    void record_submit(const std::string& task_id,
                       const std::string& task_name,
                       std::chrono::steady_clock::time_point at);

    void record_complete(const std::string& task_id,
                         TaskStatus status,
                         std::chrono::milliseconds exec_time);

    [[nodiscard]] std::vector<TaskRecord> recent(std::size_t count) const;
    [[nodiscard]] std::vector<TaskRecord> since(std::chrono::steady_clock::time_point since) const;
    [[nodiscard]] std::size_t count() const;

    void clear();

private:
    std::size_t max_size_;
    mutable std::mutex mutex_;
    std::deque<TaskRecord> entries_;
};

}  // namespace ctee
