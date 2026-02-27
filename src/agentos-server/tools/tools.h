// =============================================================================
// tools/tools.h — Tool registration declarations for agentos-server
// =============================================================================
//
// Each register_*() function adds one or more MCP tools to the server.
// Call order doesn't matter — tools are stored in a flat map by name.
//
// To add a new tool:
//   1. Create tools/mytool.cpp with a static handler function and a
//      register_mytool() function following the pattern in the existing files.
//   2. Add the .cpp to src/agentos-server/CMakeLists.txt.
//   3. Declare register_mytool() here and call it from main.cpp.
// =============================================================================

#pragma once
#include "mcp_server.h"
#include "mcp_tool.h"

// Each function registers its tool(s) into the given server.
void register_exec_tool(mcp::server& server);
void register_filesystem_tools(mcp::server& server);
void register_sysinfo_tool(mcp::server& server);
void register_process_tool(mcp::server& server);
void register_journal_tool(mcp::server& server);
void register_network_tool(mcp::server& server);
