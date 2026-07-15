include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

FetchContent_MakeAvailable(googletest)

# =============================================================================
# ctee_core_tests — reusable library tests
#
# Links ONLY against ctee_core.  Must not depend on httplib, nlohmann_json,
# or ctee_app.  These tests validate the core abstractions (Task, TaskQueue,
# ThreadPool, Worker, Scheduler, Metrics, TaskHistory) in isolation.
# =============================================================================
set(CTEE_CORE_TEST_SOURCES
    core/task_test.cpp
    core/task_queue_test.cpp
    core/worker_test.cpp
    core/thread_pool_test.cpp
    core/scheduler_test.cpp
    core/metrics_test.cpp
    core/task_history_test.cpp
    core/retry_test.cpp
    core/timeout_test.cpp
    core/dependencies_test.cpp
    core/batch_test.cpp
    core/thread_naming_test.cpp
    core/stress_test.cpp
    core/concurrency_test.cpp
    core/benchmark_test.cpp
    core/deadlock_test.cpp
    core/load_test.cpp
    core/edge_case_test.cpp
)

add_executable(ctee_core_tests ${CTEE_CORE_TEST_SOURCES})

target_include_directories(ctee_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ctee_core_tests PRIVATE
    GTest::gtest_main
    ctee_core
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(ctee_core_tests)

# =============================================================================
# ctee_app_tests — application/server tests
#
# Links against ctee_app (which includes ctee_core + httplib + nlohmann_json).
# These tests validate the HTTP server, config loading, and logger.
# =============================================================================
set(CTEE_APP_TEST_SOURCES
    core/config_test.cpp
    common/logger_test.cpp
    server/http_server_test.cpp
)

add_executable(ctee_app_tests ${CTEE_APP_TEST_SOURCES})

target_include_directories(ctee_app_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ctee_app_tests PRIVATE
    GTest::gtest_main
    ctee_app
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads
)

gtest_discover_tests(ctee_app_tests)
