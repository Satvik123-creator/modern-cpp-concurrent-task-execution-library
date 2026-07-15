#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>

namespace ctee {

enum class TaskStatus : std::uint8_t {
    Pending   = 0,
    Queued    = 1,
    Running   = 2,
    Completed = 3,
    Failed    = 4,
    Cancelled = 5,
    TimedOut  = 6
};

const char* to_string(TaskStatus status);

struct TaskResult {
    bool success = false;
    std::optional<std::string> value;
    std::exception_ptr exception;
    std::chrono::milliseconds execution_time{0};
};

}  // namespace ctee
