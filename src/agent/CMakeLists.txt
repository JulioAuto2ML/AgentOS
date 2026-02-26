set(TARGET nos-agent-run)

# yaml-cpp is required for parsing agent YAML definition files.
# Install with: sudo apt-get install -y libyaml-cpp-dev
find_package(yaml-cpp REQUIRED)

# OpenSSL is required for HTTPS (Groq, Anthropic, etc.)
# Install with: sudo apt-get install -y libssl-dev
find_package(OpenSSL REQUIRED)

# ── agent library (shared between nos-agent-run and nos-supervisor) ────────────
add_library(agent_lib STATIC
    agent_config.cpp
    agent.cpp
    llm_client.cpp
)

target_include_directories(agent_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(agent_lib PUBLIC
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# ── nos-agent-run CLI ──────────────────────────────────────────────────────────
add_executable(${TARGET} agent_run.cpp)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(${TARGET} PRIVATE agent_lib)

install(TARGETS ${TARGET} DESTINATION bin)
