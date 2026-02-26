set(TARGET nos-supervisor)

add_executable(${TARGET}
    main.cpp
    supervisor.cpp
)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/agent
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

# agent_lib brings in mcp, yaml-cpp, OpenSSL, and CPPHTTPLIB_OPENSSL_SUPPORT
target_link_libraries(${TARGET} PRIVATE agent_lib)

install(TARGETS ${TARGET} DESTINATION bin)

# ── nos-builder CLI ───────────────────────────────────────────────────────────
add_executable(nos-builder builder_main.cpp)

target_include_directories(nos-builder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/agent
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(nos-builder PRIVATE agent_lib)
install(TARGETS nos-builder DESTINATION bin)
