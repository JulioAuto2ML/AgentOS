cmake_minimum_required(VERSION 3.14)
project(NeuralOS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# ── cpp-mcp (vendored, MIT) ───────────────────────────────────────────────────
# We define the target ourselves to avoid building examples/tests from upstream.
set(CPP_MCP_DIR ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp)

add_library(mcp STATIC
    ${CPP_MCP_DIR}/src/mcp_message.cpp
    ${CPP_MCP_DIR}/src/mcp_resource.cpp
    ${CPP_MCP_DIR}/src/mcp_server.cpp
    ${CPP_MCP_DIR}/src/mcp_tool.cpp
    ${CPP_MCP_DIR}/src/mcp_sse_client.cpp
    ${CPP_MCP_DIR}/src/mcp_stdio_client.cpp
)

target_include_directories(mcp PUBLIC
    ${CPP_MCP_DIR}/include
    ${CPP_MCP_DIR}/common
)

target_link_libraries(mcp PUBLIC Threads::Threads)

add_subdirectory(src/nos-server)
add_subdirectory(src/agent)
