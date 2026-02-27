// =============================================================================
// src/agentos-supervisor/main.cpp — agentos-supervisor entry point
// =============================================================================
//
// agentos-supervisor is the agent process manager for AgentOS.
// It loads agent YAML definitions, manages AgentInstance lifecycles, and
// provides a REST API over HTTP for agentos-cli and external tools.
//
// Usage:
//   agentos-supervisor [--host H] [--port P] [--agents-dir DIR]
//
// Defaults:
//   --host        localhost
//   --port        8889
//   --agents-dir  ./agents
//
// Environment:
//   AGENTOS_LLM_URL     LLM backend (default: http://localhost:8080)
//   AGENTOS_LLM_KEY     API key (default: empty)
//   AGENTOS_SERVER_URL  agentos-server URL (default: http://localhost:8888)
//
// REST API:
//
//   GET  /health
//     → {"status":"ok","agents":3}
//
//   GET  /agents
//     → {"agents":[{"name":"sysmonitor","status":"idle","run_count":0,...}]}
//
//   GET  /agents/{name}
//     → {"name":"sysmonitor","status":"idle","model":"default",...}
//
//   POST /agents/{name}/run
//     Body: {"message":"What is the CPU usage?"}
//     → {"response":"CPU usage is 12.3% across 6 cores..."}
//     → {"error":"..."} on failure
//
//   POST /agents/reload
//     → {"loaded":3}
// =============================================================================

#include "supervisor.h"
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <thread>

using json = nlohmann::json;

static httplib::Server* g_server = nullptr;

static void signal_handler(int) {
    std::cerr << "\n[supervisor] Shutting down...\n";
    if (g_server) g_server->stop();
}

static const char* getenv_or(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return v ? v : fallback;
}

// ── Response helpers ──────────────────────────────────────────────────────────

static void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static void send_error(httplib::Response& res, const std::string& msg, int status = 500) {
    send_json(res, {{"error", msg}}, status);
}

// ── Arg parsing ───────────────────────────────────────────────────────────────

static std::string arg_value(int argc, char* argv[],
                             const std::string& flag, const std::string& def) {
    for (int i = 1; i < argc - 1; ++i)
        if (argv[i] == flag) return argv[i + 1];
    return def;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const std::string host        = arg_value(argc, argv, "--host",       "localhost");
    const int         port        = std::stoi(arg_value(argc, argv, "--port", "8889"));
    const std::string agents_dir  = arg_value(argc, argv, "--agents-dir", "./agents");
    const std::string llm_url     = getenv_or("AGENTOS_LLM_URL",    "http://localhost:8080");
    const std::string llm_key     = getenv_or("AGENTOS_LLM_KEY",    "");
    const std::string server_url  = getenv_or("AGENTOS_SERVER_URL", "http://localhost:8888");

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    Supervisor supervisor(agents_dir, server_url, llm_url, llm_key);

    int n = supervisor.load_agents();
    std::cerr << "[supervisor] Loaded " << n << " agents from " << agents_dir << "\n";

    // ── Routes ─────────────────────────────────────────────────────────────────
    httplib::Server server;
    g_server = &server;

    // GET /health
    server.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        auto agents = supervisor.list_agents();
        send_json(res, {{"status","ok"}, {"agents", static_cast<int>(agents.size())}});
    });

    // GET /agents
    server.Get("/agents", [&](const httplib::Request&, httplib::Response& res) {
        auto agents = supervisor.list_agents();
        json arr = json::array();
        for (const auto& a : agents) {
            arr.push_back({
                {"name",        a.name},
                {"description", a.description},
                {"model",       a.model},
                {"status",      a.status},
                {"run_count",   a.run_count},
                {"error_count", a.error_count},
                {"priority",    a.priority}
            });
        }
        send_json(res, {{"agents", arr}});
    });

    // GET /agents/{name}
    server.Get(R"(/agents/([^/]+))", [&](const httplib::Request& req,
                                          httplib::Response& res) {
        const std::string name = req.matches[1];
        auto snap = supervisor.agent_info(name);
        if (snap.name.empty()) {
            send_error(res, "Agent not found: " + name, 404);
            return;
        }
        send_json(res, {
            {"name",        snap.name},
            {"description", snap.description},
            {"model",       snap.model},
            {"status",      snap.status},
            {"last_error",  snap.last_error},
            {"run_count",   snap.run_count},
            {"error_count", snap.error_count},
            {"priority",    snap.priority}
        });
    });

    // POST /agents/{name}/run
    server.Post(R"(/agents/([^/]+)/run)", [&](const httplib::Request& req,
                                               httplib::Response& res) {
        const std::string name = req.matches[1];

        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            send_error(res, "Invalid JSON body", 400);
            return;
        }

        if (!body.contains("message") || !body["message"].is_string()) {
            send_error(res, "Missing 'message' field", 400);
            return;
        }

        const std::string message = body["message"].get<std::string>();
        const bool verbose = body.value("verbose", false);

        std::cerr << "[supervisor] Run: " << name << " ← \"" << message.substr(0,60) << "\"\n";

        try {
            std::string response = supervisor.run_agent(name, message, verbose);
            send_json(res, {{"response", response}});
        } catch (const std::exception& e) {
            send_error(res, e.what(), 500);
        }
    });

    // POST /agents/reload
    server.Post("/agents/reload", [&](const httplib::Request&, httplib::Response& res) {
        try {
            int n = supervisor.load_agents();
            send_json(res, {{"loaded", n}});
        } catch (const std::exception& e) {
            send_error(res, e.what(), 500);
        }
    });

    std::cerr << "[supervisor] Starting on " << host << ":" << port << "\n";
    std::cerr << "[supervisor] agentos-server: " << server_url << "\n";
    std::cerr << "[supervisor] LLM: " << llm_url << "\n";

    if (!server.listen(host, port)) {
        std::cerr << "[supervisor] Failed to bind on " << host << ":" << port << "\n";
        return 1;
    }

    return 0;
}
