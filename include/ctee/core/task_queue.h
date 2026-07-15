#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ctee/core/task.h"

namespace ctee::detail {

class TaskQueue {
public:
    explicit TaskQueue(std::size_t capacity = 10000);

    void push(Task task);
    std::optional<Task> pop();

    bool cancel(const std::string& task_id);
    bool is_cancelled(const std::string& task_id) const;
    void mark_completed(const std::string& task_id);
    void clear();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool is_full() const noexcept;

private:
    std::vector<Task> heap_;
    std::unordered_set<std::string> cancelled_ids_;
    std::unordered_set<std::string> completed_ids_;
    std::size_t capacity_;
};

}  // namespace ctee::detail
