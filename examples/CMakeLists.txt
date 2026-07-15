# Examples for the Concurrent Task Execution Engine.
#
# Build with:
#   cmake --build build --target example_basic_pool

add_executable(example_basic_pool basic_pool.cpp)
target_link_libraries(example_basic_pool PRIVATE ctee_core Threads::Threads)
target_include_directories(example_basic_pool PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(example_basic_pool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    CXX_STANDARD 20
)
