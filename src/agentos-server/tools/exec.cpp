// =============================================================================
// tools/exec.cpp — MCP tool: exec
// =============================================================================
//
// Exposes shell command execution to LLM agents via MCP.
//
// Design decisions:
//   - Uses `timeout(1)` from coreutils rather than SIGALRM/threads so that
//     the child process *and* its descendants are killed when time is up.
//   - Single-quotes the user command and escapes embedded single-quotes so
//     that it runs literally in /bin/sh without injection risk.
//   - stdout and stderr are merged (2>&1) — agents rarely need to distinguish
//     them and it simplifies log reading.
//   - working_dir is optional; if provided, a `cd` is prepended.
//   - exit_code 124 is timeout(1)'s sentinel for "killed due to timeout".
//
// Security note:
//   agentos-server runs with the same privileges as the user who starts it.
//   Restrict agent tool lists in agent YAML to avoid unwanted exec calls.
// =============================================================================

#include "tools.h"
#include "mcp_message.h"
#include <string>
#include <stdexcept>
#include <sys/wait.h>
#include <array>
#include <cstdio>
#include <sstream>

// Run a shell command with a timeout (via the system `timeout` utility).
// Returns stdout+stderr, exit code, and whether it timed out.
static mcp::json exec_handler(const mcp::json& params, const std::string&) {
    if (!params.contains("command") || !params["command"].is_string())
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing or invalid 'command'");

    const std::string command     = params["command"].get<std::string>();
    const int         timeout_ms  = params.value("timeout_ms", 10000);
    const std::string working_dir = params.value("working_dir", std::string{});
    const int         timeout_s   = std::max(1, timeout_ms / 1000);

    // Build: [cd working_dir &&] timeout N /bin/sh -c 'command' 2>&1
    // We single-quote the user command and escape any single-quotes inside it.
    auto shell_quote = [](const std::string& s) -> std::string {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else           out += c;
        }
        out += "'";
        return out;
    };

    std::string cmd;
    if (!working_dir.empty())
        cmd = "cd " + shell_quote(working_dir) + " && ";
    cmd += "timeout " + std::to_string(timeout_s) + " /bin/sh -c " + shell_quote(command) + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        throw mcp::mcp_exception(mcp::error_code::internal_error, "popen failed");

    std::ostringstream output;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), buf.size(), pipe))
        output << buf.data();

    int raw_status = pclose(pipe);
    int exit_code  = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;
    bool timed_out = (exit_code == 124); // timeout(1) exits with 124

    mcp::json result = {
        {"stdout",    output.str()},
        {"exit_code", exit_code},
        {"timed_out", timed_out}
    };

    return mcp::json::array({{
        {"type", "text"},
        {"text", result.dump()}
    }});
}

void register_exec_tool(mcp::server& server) {
    auto tool = mcp::tool_builder("exec")
        .with_description(
            "Execute a shell command and return stdout, stderr, and exit code. "
            "Commands run in /bin/sh and are killed after timeout_ms milliseconds.")
        .with_string_param("command",     "Shell command to execute")
        .with_number_param("timeout_ms",  "Timeout in milliseconds (default: 10000)", false)
        .with_string_param("working_dir", "Working directory (default: current)", false)
        .build();

    server.register_tool(tool, exec_handler);
}
