cmake_minimum_required(VERSION 3.14)
project(NeuralOS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# cpp-mcp: disable examples and tests to keep the build fast
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Override cpp-mcp's add_subdirectory(examples) — we patch via EXCLUDE_FROM_ALL
add_subdirectory(third-party/cpp-mcp EXCLUDE_FROM_ALL)

# Expose cpp-mcp headers to all sub-targets
include_directories(
    third-party/cpp-mcp/include
    third-party/cpp-mcp/common
)

add_subdirectory(src/nos-server)
