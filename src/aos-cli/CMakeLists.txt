add_executable(aos main.cpp)

target_include_directories(aos PRIVATE
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

# mcp brings CPPHTTPLIB_OPENSSL_SUPPORT + OpenSSL — aos-cli uses httplib directly
target_link_libraries(aos PRIVATE mcp)

install(TARGETS aos DESTINATION bin)
