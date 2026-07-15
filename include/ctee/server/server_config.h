#pragma once

#include <cstdint>
#include <string>

namespace ctee {

struct ServerConfig {
    std::uint16_t port = 8080;
    std::string host = "0.0.0.0";
};

}  // namespace ctee
