// =============================================================================
// tools/script_tools.h — Dynamic script-based MCP tool loader
// =============================================================================
//
// Scans a directory for Python (.py) and shell (.sh) scripts and registers
// each one as an MCP tool without recompiling agentos-server.
//
// Script metadata format (comment block at the top of the script):
//
//   # @tool name:        my_tool
//   # @tool description: Does something useful for agents.
//   # @tool param:       city    string  required  The city name
//   # @tool param:       units   string  optional  Temperature units
//
// Param line format:
//   # @tool param: <name> <type> <required|optional> <description>
//
// Supported types: string, number, boolean, integer
//
// When called, the tool:
//   1. Writes the JSON params object to the script's stdin
//   2. Executes: python3 <script> (for .py) or bash <script> (for .sh)
//   3. Reads stdout as the result (plain text or JSON)
//   4. Returns it as MCP tool content
//
// Usage:
//   register_script_tools(server, "./tools");
//   register_script_tools(server, getenv("AGENTOS_TOOLS_DIR") ?: "./tools");
//
// Env var: AGENTOS_TOOLS_DIR (overrides the dir passed to register_script_tools)
// =============================================================================

#pragma once
#include "mcp_server.h"
#include <string>

// Scan dir for *.py and *.sh files, parse their @tool metadata, and
// register each as an MCP tool in server.
// Logs to stderr. Does nothing if dir doesn't exist.
void register_script_tools(mcp::server& server, const std::string& tools_dir);
