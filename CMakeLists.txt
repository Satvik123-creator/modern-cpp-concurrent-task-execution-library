cmake_minimum_required(VERSION 3.20)

project(ConcurrentTaskExecutionEngine
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Reusable concurrent execution framework in Modern C++20"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CTEE_BUILD_TESTS "Build unit tests" ON)
option(CTEE_BUILD_EXAMPLES "Build examples" ON)
option(CTEE_BUILD_FRONTEND "Build frontend assets" OFF)

include(FetchContent)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

FetchContent_MakeAvailable(httplib json)

find_package(Threads REQUIRED)

add_subdirectory(src)

if(CTEE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CTEE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
