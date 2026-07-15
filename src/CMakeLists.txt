# ── Compiler flags (shared by all targets) ────────────────────────────────
if(MSVC)
    set(CTEE_COMPILE_OPTIONS /W4)
else()
    set(CTEE_COMPILE_OPTIONS -Wall -Wextra -Wpedantic)
endif()

# ── Core library (reusable, no HTTP/JSON deps) ───────────────────────────
#
# This is the standalone, linkable library.  Any external project can
# link ctee_core and get the thread pool, scheduler, metrics, task
# history, and all core abstractions without pulling in httplib or
# nlohmann_json.
# ─────────────────────────────────────────────────────────────────────────
add_library(ctee_core STATIC
    core/task.cpp
    core/task_queue.cpp
    core/worker.cpp
    core/thread_pool.cpp
    core/scheduler.cpp
    core/metrics.cpp
    core/task_history.cpp
)

target_include_directories(ctee_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ctee_core PUBLIC
    Threads::Threads
)

target_compile_options(ctee_core PUBLIC ${CTEE_COMPILE_OPTIONS})

# ── Application library (server + config + logger) ───────────────────────
#
# Depends on ctee_core.  Contains the HTTP server, REST route handler,
# task registry, async logger, and JSON config loader.  Link this when
# you want the full application stack.
# ─────────────────────────────────────────────────────────────────────────
add_library(ctee_app STATIC
    server/http_server.cpp
    server/route_handler.cpp
    server/task_registry.cpp
    common/logger.cpp
    common/config.cpp
)

target_include_directories(ctee_app PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ctee_app PUBLIC
    ctee_core
    httplib::httplib
    nlohmann_json::nlohmann_json
)

target_compile_options(ctee_app PRIVATE ${CTEE_COMPILE_OPTIONS})

# ── Executable ───────────────────────────────────────────────────────────
add_executable(ctee main.cpp)

target_link_libraries(ctee PRIVATE ctee_app)

set_target_properties(ctee PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ── Install targets ──────────────────────────────────────────────────────
include(GNUInstallDirs)

# ctee_core is the standalone library with zero external deps (just pthreads).
install(TARGETS ctee_core
    EXPORT CTEETargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ctee
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
        PATTERN "*.h"
)

install(EXPORT CTEETargets
    FILE CTEETargets.cmake
    NAMESPACE CTEE::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CTEE
)

# ctee_app and ctee executable (not exported — depends on httplib/nlohmann_json)
install(TARGETS ctee_app ctee
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ── Package config ───────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/CTEEConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CTEEConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CTEE
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CTEEConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CTEEConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CTEEConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CTEE
)
