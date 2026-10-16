#include "ctee/core/task_queue.h"

using namespace std;

namespace ctee::detail {

TaskQueue::TaskQueue(size_t capacity) : capacity_(capacity) {}

void TaskQueue::push(Task task) {
    if (heap_.size() >= capacity_) {
        return;
    }
    heap_.push_back(move(task));
    push_heap(heap_.begin(), heap_.end());
}

optional<Task> TaskQueue::pop() {
    // Try to find a task whose dependencies are all met.
    // Tasks whose deps aren't satisfied are put aside and pushed back afterwards.
    // This is O(n) in worst case but fine for typical queue sizes and avoids
    // complex DAG scheduling.
    vector<Task> deferred;
    optional<Task> result;

    while (!heap_.empty()) {
        pop_heap(heap_.begin(), heap_.end());
        Task task = move(heap_.back());
        heap_.pop_back();

        if (cancelled_ids_.count(task.id()) > 0) {
            continue;
        }

        // Check if all dependencies are satisfied
        bool deps_met = true;
        for (const auto& dep_id : task.depends_on()) {
            if (completed_ids_.count(dep_id) == 0) {
                deps_met = false;
                break;
            }
        }

        if (!deps_met) {
            deferred.push_back(move(task));
            continue;
        }

        result = move(task);
        break;
    }

    // Push back deferred tasks into the heap
    for (auto& t : deferred) {
        heap_.push_back(move(t));
        push_heap(heap_.begin(), heap_.end());
    }

    return result;
}

bool TaskQueue::cancel(const string& task_id) {
    cancelled_ids_.insert(task_id);
    return true;
}

bool TaskQueue::is_cancelled(const string& task_id) const {
    return cancelled_ids_.count(task_id) > 0;
}

void TaskQueue::mark_completed(const string& task_id) {
    completed_ids_.insert(task_id);
}

void TaskQueue::clear() {
    heap_.clear();
    cancelled_ids_.clear();
    completed_ids_.clear();
}

bool TaskQueue::empty() const noexcept {
    return heap_.empty();
}

std::size_t TaskQueue::size() const noexcept {
    return heap_.size();
}

bool TaskQueue::is_full() const noexcept {
    return heap_.size() >= capacity_;
}

}  // namespace ctee::detail
