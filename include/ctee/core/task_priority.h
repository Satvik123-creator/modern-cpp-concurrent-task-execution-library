#pragma once

#include <cstdint>
#include <string>

namespace ctee {

enum class TaskPriority : std::uint8_t {
    Low      = 0,
    Normal   = 1,
    High     = 2,
    Critical = 3
};

const char* to_string(TaskPriority priority);

}  // namespace ctee
