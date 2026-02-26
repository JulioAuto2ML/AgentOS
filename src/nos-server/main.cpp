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

    mcp::server::configuration conf;
    conf.host    = host;
    conf.port    = port;
    conf.name    = "nos-server";
    conf.version = "0.1.0";

    mcp::server server(conf);
    server.set_server_info("nos-server", "0.1.0");
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
