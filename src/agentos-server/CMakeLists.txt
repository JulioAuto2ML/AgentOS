set(TARGET agentos-server)

add_executable(${TARGET}
    main.cpp
    tools/exec.cpp
    tools/filesystem.cpp
    tools/sysinfo.cpp
    tools/process.cpp
    tools/journal.cpp
    tools/network.cpp
)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp/common
)

target_link_libraries(${TARGET} PRIVATE mcp Threads::Threads)

# agentos-server only targets Linux
if(NOT UNIX)
    message(FATAL_ERROR "agentos-server requires a Linux/POSIX environment")
endif()

install(TARGETS ${TARGET} DESTINATION bin)
