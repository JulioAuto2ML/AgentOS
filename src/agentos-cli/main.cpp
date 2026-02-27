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
//   start               Start agentos-server + agentos-supervisor as background daemons
//   stop                Stop both daemons (SIGTERM, then SIGKILL if needed)
//   restart             Stop then start both daemons
//   status              Show full stack status (reachability check)
//   agents              List all loaded agents with status
//   run <agent> <msg>   Run an agent with a message
//   ask <msg>           Run the default agent (AGENTOS_DEFAULT_AGENT or "sysmonitor")
//   reload              Reload agents from disk
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
#include <fstream>
#include <string>
#include <regex>
#include <cstdlib>
#include <iomanip>
#include <thread>
#include <chrono>
// POSIX (Linux)
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>

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

// ── Daemon management ─────────────────────────────────────────────────────────

// Return the directory that contains the currently-running agentos binary.
// Used to find sibling binaries (agentos-server, agentos-supervisor).
static std::string self_dir() {
    char buf[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string path(buf, len);
    auto pos = path.rfind('/');
    return (pos == std::string::npos) ? "." : path.substr(0, pos);
}

static std::string pid_path(const std::string& name) {
    return "/tmp/agentos-" + name + ".pid";
}

static std::string log_path(const std::string& name) {
    return "/tmp/agentos-" + name + ".log";
}

static pid_t read_pid(const std::string& name) {
    std::ifstream f(pid_path(name));
    pid_t pid = 0;
    if (f) f >> pid;
    return pid;
}

static void write_pid(const std::string& name, pid_t pid) {
    std::ofstream f(pid_path(name));
    f << pid << "\n";
}

static bool process_alive(pid_t pid) {
    return pid > 0 && (kill(pid, 0) == 0);
}

// Fork the given binary into a background daemon.
// stdout/stderr are redirected to log_path(name).
// Returns the child PID, or throws on error.
static pid_t launch_daemon(const std::string& binary, const std::string& name,
                            const std::vector<std::string>& extra_args = {}) {
    pid_t pid = fork();
    if (pid < 0)
        throw std::runtime_error(std::string("fork failed: ") + strerror(errno));

    if (pid == 0) {
        // Child: new session, redirect I/O to log file, exec binary
        setsid();

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }

        int logfd = open(log_path(name).c_str(),
                         O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }

        // Build argv
        std::vector<std::string> args_str = {binary};
        for (const auto& a : extra_args) args_str.push_back(a);
        std::vector<char*> argv_ptrs;
        for (auto& s : args_str) argv_ptrs.push_back(s.data());
        argv_ptrs.push_back(nullptr);

        execv(binary.c_str(), argv_ptrs.data());
        _exit(1);
    }

    return pid;
}

// Stop a daemon by name. Returns true if process was found and signalled.
static bool stop_daemon(const std::string& name) {
    pid_t pid = read_pid(name);
    if (!process_alive(pid)) {
        std::remove(pid_path(name).c_str());
        return false;
    }
    kill(pid, SIGTERM);
    // Wait up to 5 seconds for graceful exit
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!process_alive(pid)) break;
    }
    // Force-kill if still alive
    if (process_alive(pid)) kill(pid, SIGKILL);
    std::remove(pid_path(name).c_str());
    return true;
}

static int cmd_start() {
    const std::string dir = self_dir();

    struct Daemon {
        std::string binary_name;
        std::string service_name;
    };

    std::vector<Daemon> daemons = {
        {"agentos-server",     "server"},
        {"agentos-supervisor", "supervisor"},
    };

    bool any_started = false;
    for (const auto& d : daemons) {
        pid_t existing = read_pid(d.service_name);
        if (process_alive(existing)) {
            std::cout << d.service_name << " already running (pid " << existing << ")\n";
            continue;
        }

        std::string binary = dir + "/" + d.binary_name;
        if (access(binary.c_str(), X_OK) != 0) {
            std::cerr << "Cannot find " << binary << " — is it installed?\n";
            continue;
        }

        pid_t pid = launch_daemon(binary, d.service_name);
        write_pid(d.service_name, pid);
        std::cout << "Started " << d.service_name << " (pid " << pid
                  << ", log: " << log_path(d.service_name) << ")\n";
        any_started = true;
    }

    if (any_started) {
        // Brief pause so processes have time to bind their ports
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

static int cmd_stop() {
    for (const std::string& name : {"supervisor", "server"}) {
        if (stop_daemon(name))
            std::cout << "Stopped " << name << "\n";
        else
            std::cout << name << " was not running\n";
    }
    return 0;
}

static int cmd_restart() {
    cmd_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return cmd_start();
}

static void print_help(const char* prog) {
    std::cerr << "AgentOS CLI\n\n"
              << "Usage: " << prog << " <command> [args]\n\n"
              << "Commands:\n"
              << "  start                    Start agentos-server and agentos-supervisor\n"
              << "  stop                     Stop both daemons\n"
              << "  restart                  Stop then start both daemons\n"
              << "  status                   Show stack health\n"
              << "  agents                   List loaded agents\n"
              << "  run <agent> <message>    Run an agent\n"
              << "  ask <message>            Run the default agent\n"
              << "  reload                   Reload agents from disk\n\n"
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
        if (cmd == "start") {
            return cmd_start();

        } else if (cmd == "stop") {
            return cmd_stop();

        } else if (cmd == "restart") {
            return cmd_restart();

        } else if (cmd == "agents") {
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
