add_executable(nos main.cpp)

target_include_directories(nos PRIVATE
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

# mcp brings CPPHTTPLIB_OPENSSL_SUPPORT + OpenSSL — nos-cli uses httplib directly
target_link_libraries(nos PRIVATE mcp)

install(TARGETS nos DESTINATION bin)
