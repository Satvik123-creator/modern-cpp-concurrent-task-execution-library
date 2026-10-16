#include "ctee/core/task_history.h"

using namespace std;
namespace ch = std::chrono;

namespace ctee {

TaskHistory::TaskHistory(size_t max_size) : max_size_(max_size) {}

void TaskHistory::record(const TaskRecord& entry) {
    lock_guard lock(mutex_);
    entries_.push_back(entry);
    while (entries_.size() > max_size_) {
        entries_.pop_front();
    }
}

vector<TaskRecord> TaskHistory::recent(size_t count) const {
    lock_guard lock(mutex_);
    vector<TaskRecord> result;
    auto start = entries_.size() > count ? entries_.size() - count : 0;
    result.reserve(entries_.size() - start);
    for (auto i = start; i < entries_.size(); ++i) {
        result.push_back(entries_[i]);
    }
    return result;
}

vector<TaskRecord> TaskHistory::since(ch::steady_clock::time_point since) const {
    lock_guard lock(mutex_);
    vector<TaskRecord> result;
    for (const auto& entry : entries_) {
        if (entry.completed_at >= since) {
            result.push_back(entry);
        }
    }
    return result;
}

size_t TaskHistory::count() const {
    lock_guard lock(mutex_);
    return entries_.size();
}

void TaskHistory::clear() {
    lock_guard lock(mutex_);
    entries_.clear();
}

void TaskHistory::record_submit(const string& task_id,
                                const string& task_name,
                                ch::steady_clock::time_point at) {
    TaskRecord entry;
    entry.task_id = task_id;
    entry.task_name = task_name;
    entry.status = TaskStatus::Queued;
    entry.submitted_at = at;
    record(entry);
}

void TaskHistory::record_complete(const string& task_id,
                                  TaskStatus status,
                                  ch::milliseconds exec_time) {
    lock_guard lock(mutex_);
    for (auto& e : entries_) {
        if (e.task_id == task_id) {
            e.status = status;
            e.completed_at = ch::steady_clock::now();
            e.execution_time = exec_time;
            return;
        }
    }
}

}  // namespace ctee
