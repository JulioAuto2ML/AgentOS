add_executable(agentos main.cpp)

target_include_directories(agentos PRIVATE
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

# mcp brings CPPHTTPLIB_OPENSSL_SUPPORT + OpenSSL — agentos-cli uses httplib directly
target_link_libraries(agentos PRIVATE mcp)

install(TARGETS agentos DESTINATION bin)
