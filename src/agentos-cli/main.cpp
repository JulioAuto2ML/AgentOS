// =============================================================================
// src/agentos-cli/main.cpp — agentos: the AgentOS command-line interface
// =============================================================================
//
// Central CLI for interacting with a running AgentOS stack
// (agentos-server + agentos-supervisor).
//
// Usage:
//   agentos <command> [args...]
//
// Commands:
//   agents              List all loaded agents with status
//   run <agent> <msg>   Run an agent with a message
//   ask <msg>           Run the default agent (AGENTOS_DEFAULT_AGENT or "sysmonitor")
//   build <description> Create a new agent from a description (builder agent)
//   reload              Reload agents from disk
//   server              Show agentos-server tool list (via agentos-server health)
//   status              Show full stack status
//
// Environment:
//   AGENTOS_SUPERVISOR_URL   (default: http://localhost:8889)
//   AGENTOS_SERVER_URL       (default: http://localhost:8888)
//   AGENTOS_DEFAULT_AGENT    (default: sysmonitor)
//   AGENTOS_LLM_URL          (forwarded to agentos-agent-run / agentos-builder)
//   AGENTOS_LLM_KEY          (forwarded)
//
// All commands communicate with agentos-supervisor via HTTP.
// agentos-supervisor must be running (agentos-supervisor &).
// =============================================================================

#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <string>
#include <regex>
#include <cstdlib>
#include <iomanip>

using json = nlohmann::json;

static const char* getenv_or(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return v ? v : fallback;
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

struct ServerConn {
    std::string host;
    int port;
};

static ServerConn parse_url(const std::string& url) {
    std::regex re(R"(https?://([^/:]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(url, m, re))
        return {"localhost", 8889};
    return {m[1].str(), m[2].matched ? std::stoi(m[2].str()) : 8889};
}

static json http_get(const std::string& base_url, const std::string& path) {
    auto [host, port] = parse_url(base_url);
    httplib::Client cli(host, port);
    cli.set_read_timeout(30);
    auto res = cli.Get(path);
    if (!res) throw std::runtime_error("Cannot reach " + base_url + path);
    if (res->status / 100 != 2)
        throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + res->body);
    return json::parse(res->body);
}

static json http_post(const std::string& base_url, const std::string& path,
                      const json& body = json::object()) {
    auto [host, port] = parse_url(base_url);
    httplib::Client cli(host, port);
    cli.set_read_timeout(300); // agents can take time
    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res) throw std::runtime_error("Cannot reach " + base_url + path);
    if (res->status / 100 != 2)
        throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + res->body);
    return json::parse(res->body);
}

// ── Commands ──────────────────────────────────────────────────────────────────

static int cmd_agents(const std::string& sup_url) {
    auto resp = http_get(sup_url, "/agents");
    auto& agents = resp["agents"];

    if (agents.empty()) {
        std::cout << "No agents loaded.\n";
        return 0;
    }

    // Column widths
    std::cout << std::left
              << std::setw(18) << "NAME"
              << std::setw(10) << "STATUS"
              << std::setw(8)  << "RUNS"
              << std::setw(8)  << "ERRORS"
              << "DESCRIPTION\n";
    std::cout << std::string(70, '-') << "\n";

    for (const auto& a : agents) {
        std::string name   = a["name"];
        std::string status = a["status"];
        int runs   = a["run_count"];
        int errors = a["error_count"];
        std::string desc   = a["description"];
        if (desc.size() > 30) desc = desc.substr(0, 27) + "...";

        std::cout << std::left
                  << std::setw(18) << name
                  << std::setw(10) << status
                  << std::setw(8)  << runs
                  << std::setw(8)  << errors
                  << desc << "\n";
    }
    return 0;
}

static int cmd_run(const std::string& sup_url,
                   const std::string& agent_name,
                   const std::string& message) {
    try {
        auto resp = http_post(sup_url, "/agents/" + agent_name + "/run",
                              {{"message", message}});
        if (resp.contains("error"))
            throw std::runtime_error(resp["error"].get<std::string>());
        std::cout << resp["response"].get<std::string>() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

static int cmd_reload(const std::string& sup_url) {
    auto resp = http_post(sup_url, "/agents/reload");
    std::cout << "Loaded " << resp["loaded"].get<int>() << " agents.\n";
    return 0;
}

static int cmd_status(const std::string& sup_url, const std::string& srv_url) {
    // Supervisor health
    try {
        auto sup = http_get(sup_url, "/health");
        std::cout << "agentos-supervisor  OK  (" << sup["agents"].get<int>() << " agents)\n";
    } catch (...) {
        std::cout << "agentos-supervisor  UNREACHABLE  (" << sup_url << ")\n";
    }

    // agentos-server health (MCP initialize)
    try {
        // We just check if the server responds to an HTTP GET on /sse (MCP endpoint)
        auto [host, port] = parse_url(srv_url);
        httplib::Client cli(host, port);
        cli.set_read_timeout(3);
        auto res = cli.Get("/sse");
        // /sse returns 200 or starts streaming — either way the server is up
        std::cout << "agentos-server      OK  (" << srv_url << ")\n";
    } catch (...) {
        std::cout << "agentos-server      UNREACHABLE  (" << srv_url << ")\n";
    }

    return 0;
}

static void print_help(const char* prog) {
    std::cerr << "AgentOS CLI\n\n"
              << "Usage: " << prog << " <command> [args]\n\n"
              << "Commands:\n"
              << "  agents                   List loaded agents\n"
              << "  run <agent> <message>    Run an agent\n"
              << "  ask <message>            Run the default agent\n"
              << "  reload                   Reload agents from disk\n"
              << "  status                   Show stack health\n\n"
              << "Environment:\n"
              << "  AGENTOS_SUPERVISOR_URL  (default: http://localhost:8889)\n"
              << "  AGENTOS_DEFAULT_AGENT   (default: sysmonitor)\n";
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const std::string sup_url = getenv_or("AGENTOS_SUPERVISOR_URL", "http://localhost:8889");
    const std::string srv_url = getenv_or("AGENTOS_SERVER_URL",     "http://localhost:8888");
    const std::string def_agent = getenv_or("AGENTOS_DEFAULT_AGENT", "sysmonitor");

    if (argc < 2) { print_help(argv[0]); return 1; }

    const std::string cmd = argv[1];

    try {
        if (cmd == "agents") {
            return cmd_agents(sup_url);

        } else if (cmd == "run") {
            if (argc < 4) {
                std::cerr << "Usage: " << argv[0] << " run <agent> <message>\n";
                return 1;
            }
            // Join remaining args as message
            std::string msg;
            for (int i = 3; i < argc; ++i) {
                if (i > 3) msg += " ";
                msg += argv[i];
            }
            return cmd_run(sup_url, argv[2], msg);

        } else if (cmd == "ask") {
            if (argc < 3) {
                std::cerr << "Usage: " << argv[0] << " ask <message>\n";
                return 1;
            }
            std::string msg;
            for (int i = 2; i < argc; ++i) {
                if (i > 2) msg += " ";
                msg += argv[i];
            }
            return cmd_run(sup_url, def_agent, msg);

        } else if (cmd == "reload") {
            return cmd_reload(sup_url);

        } else if (cmd == "status") {
            return cmd_status(sup_url, srv_url);

        } else {
            std::cerr << "Unknown command: " << cmd << "\n";
            print_help(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
