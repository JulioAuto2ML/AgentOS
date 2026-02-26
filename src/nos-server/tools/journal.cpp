// =============================================================================
// tools/journal.cpp — MCP tool: journal_query
// =============================================================================
//
// Gives agents access to system log entries via journalctl or syslog.
//
// Strategy:
//   1. Check for /usr/bin/journalctl or /bin/journalctl (systemd).
//   2. If found, run: journalctl --no-pager -n <lines> [-u unit] [--since ...] [-p level]
//   3. If not found (SysV init, Alpine, etc.), fall back to:
//              tail -n <lines> /var/log/syslog [| grep -i unit]
//
// Parameters mirror the most common journalctl flags:
//   unit  → -u  (e.g. "nginx.service" or just "nginx")
//   since → --since (e.g. "1 hour ago", "2025-01-01 12:00:00")
//   level → -p  (emerg, alert, crit, err, warning, notice, info, debug)
//   lines → -n  (default 50)
//
// Output is returned as a single "output" string so agents can grep/parse it
// further via the exec tool if needed.
// =============================================================================

#include "tools.h"
#include "mcp_message.h"
#include <cstdio>
#include <sstream>
#include <string>
#include <array>
#include <filesystem>

// Queries system logs. Uses journalctl if available, falls back to /var/log/syslog.

static bool has_journalctl() {
    return std::filesystem::exists("/usr/bin/journalctl") ||
           std::filesystem::exists("/bin/journalctl");
}

static std::string run_capture(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::ostringstream ss;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), buf.size(), pipe))
        ss << buf.data();
    pclose(pipe);
    return ss.str();
}

static mcp::json journal_handler(const mcp::json& params, const std::string&) {
    const std::string unit  = params.value("unit",  std::string{});
    const int         lines = params.value("lines", 50);
    const std::string since = params.value("since", std::string{}); // e.g. "1 hour ago"
    const std::string level = params.value("level", std::string{}); // err, warning, info…

    std::string output;

    if (has_journalctl()) {
        std::string cmd = "journalctl --no-pager -n " + std::to_string(lines);
        if (!unit.empty())  cmd += " -u " + unit;
        if (!since.empty()) cmd += " --since '" + since + "'";
        if (!level.empty()) cmd += " -p " + level;
        cmd += " 2>&1";
        output = run_capture(cmd);
    } else {
        // Fallback: tail syslog
        std::string cmd = "tail -n " + std::to_string(lines) + " /var/log/syslog 2>/dev/null";
        if (!unit.empty())
            cmd += " | grep -i '" + unit + "'";
        output = run_capture(cmd);
        if (output.empty())
            output = "(journalctl not found and /var/log/syslog not readable)";
    }

    mcp::json result = {
        {"lines",   lines},
        {"unit",    unit.empty()  ? "(all)" : unit},
        {"since",   since.empty() ? "(all)" : since},
        {"output",  output}
    };

    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

void register_journal_tool(mcp::server& server) {
    auto tool = mcp::tool_builder("journal_query")
        .with_description(
            "Query system logs via journalctl (falls back to /var/log/syslog). "
            "Filter by service unit, time range, and log level.")
        .with_string_param("unit",  "Systemd service unit name, e.g. 'nginx'",         false)
        .with_number_param("lines", "Number of lines to return (default: 50)",          false)
        .with_string_param("since", "Time range, e.g. '1 hour ago', '2024-01-01'",     false)
        .with_string_param("level", "Log level filter: err, warning, info, debug",      false)
        .build();

    server.register_tool(tool, journal_handler);
}
