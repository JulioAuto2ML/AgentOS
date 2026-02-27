set(TARGET aos-supervisor)

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

# ── aos-builder CLI ───────────────────────────────────────────────────────────
add_executable(aos-builder builder_main.cpp)

target_include_directories(aos-builder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/agent
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(aos-builder PRIVATE agent_lib)
install(TARGETS aos-builder DESTINATION bin)
