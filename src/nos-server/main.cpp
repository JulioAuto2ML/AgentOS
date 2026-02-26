// =============================================================================
// src/nos-server/main.cpp — Entry point for nos-server
// =============================================================================
//
// nos-server is an MCP (Model Context Protocol) server that exposes Linux OS
// primitives as callable tools for LLM agents.
//
// Architecture:
//   - Uses cpp-mcp (vendored, MIT) for JSON-RPC 2.0 over HTTP/SSE.
//   - Each "tool" is a stateless function: (params_json) → result_json.
//   - Tools are registered at startup; the server handles MCP sessions.
//   - Graceful shutdown on SIGINT/SIGTERM (Ctrl-C or systemd stop).
//
// Tools registered (see tools/):
//   exec           — Run a shell command with timeout
//   read_file      — Read a file (with offset/limit)
//   write_file     — Write/append to a file
//   list_dir       — List directory entries
//   sysinfo        — CPU, memory, disk, uptime snapshot
//   process_list   — Running processes sorted by memory
//   journal_query  — System log entries via journalctl
//   network_info   — Network interfaces, IPs, and traffic counters
//
// Usage:
//   nos-server [--host <host>] [--port <port>]
//   Defaults: localhost:8888
// =============================================================================

#include "mcp_server.h"
#include "tools/tools.h"
#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>

static mcp::server* g_server = nullptr;

static void handle_signal(int) {
    std::cout << "\n[nos-server] Shutting down...\n";
    if (g_server) g_server->stop();
}

static void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "  --host <host>    Bind address (default: localhost)\n"
        << "  --port <port>    Port to listen on (default: 8888)\n"
        << "  --help           Show this message\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int         port = 8888;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[nos-server] Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    mcp::server server(host, port, "nos-server", "0.1.0");
    server.set_capabilities({{"tools", mcp::json::object()}});

    // Register all OS tools
    register_exec_tool(server);
    register_filesystem_tools(server);
    register_sysinfo_tool(server);
    register_process_tool(server);
    register_journal_tool(server);
    register_network_tool(server);

    // Print registered tools
    std::cout << "[nos-server] Starting on " << host << ":" << port << "\n";
    std::cout << "[nos-server] Registered tools:\n";
    for (const auto& tool : server.get_tools())
        std::cout << "  - " << tool.name << ": " << tool.description.substr(0, 60) << "...\n";

    // Graceful shutdown on SIGINT / SIGTERM
    g_server = &server;
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    server.start(/*blocking=*/true);
    return 0;
}
