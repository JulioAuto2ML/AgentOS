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
