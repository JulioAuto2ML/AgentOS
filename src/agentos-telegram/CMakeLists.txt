# =============================================================================
# src/agentos-telegram/CMakeLists.txt
# =============================================================================

add_executable(agentos-telegram main.cpp)

target_include_directories(agentos-telegram PRIVATE
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

# mcp propagates CPPHTTPLIB_OPENSSL_SUPPORT + OpenSSL (needed for Telegram HTTPS)
target_link_libraries(agentos-telegram PRIVATE mcp)

install(TARGETS agentos-telegram DESTINATION bin)
