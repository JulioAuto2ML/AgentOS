set(TARGET agentos-agent-run)

# yaml-cpp is required for parsing agent YAML definition files.
# Install with: sudo apt-get install -y libyaml-cpp-dev
find_package(yaml-cpp REQUIRED)

# Note: OpenSSL is NOT found here — it is already a PUBLIC dependency of the
# `mcp` target (defined in the root CMakeLists.txt) and propagates automatically.

# ── agent library (shared between agentos-agent-run and agentos-supervisor) ────────────
add_library(agent_lib STATIC
    agent_config.cpp
    agent.cpp
    agent_memory.cpp
    llm_client.cpp
)

target_include_directories(agent_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(agent_lib PUBLIC
    mcp              # vendored cpp-mcp; propagates OpenSSL + CPPHTTPLIB_OPENSSL_SUPPORT
    yaml-cpp
    Threads::Threads
)

# ── agentos-agent-run CLI ──────────────────────────────────────────────────────────
add_executable(${TARGET} agent_run.cpp)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(${TARGET} PRIVATE agent_lib)

install(TARGETS ${TARGET} DESTINATION bin)
