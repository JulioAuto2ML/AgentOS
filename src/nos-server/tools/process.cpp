#include "tools.h"
#include "mcp_message.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>

namespace fs = std::filesystem;

struct ProcInfo {
    int         pid;
    std::string name;
    char        state;      // R=running S=sleeping D=disk_wait Z=zombie
    long        rss_kb;     // resident set size in KB
    float       cpu_pct;    // approximated from utime+stime / uptime
};

static ProcInfo read_proc_entry(const fs::path& proc_pid) {
    ProcInfo info{};
    info.pid = std::stoi(proc_pid.filename().string());

    // /proc/PID/stat: pid (name) state ... utime stime ... rss
    std::ifstream stat_file(proc_pid / "stat");
    if (!stat_file) return info;

    std::string stat_line;
    std::getline(stat_file, stat_line);

    // Extract name (between first '(' and last ')')
    auto name_start = stat_line.find('(');
    auto name_end   = stat_line.rfind(')');
    if (name_start == std::string::npos || name_end == std::string::npos) return info;

    info.name  = stat_line.substr(name_start + 1, name_end - name_start - 1);
    info.state = stat_line[name_end + 2];

    // Fields after ): split by space
    std::istringstream ss(stat_line.substr(name_end + 2));
    std::vector<std::string> fields;
    std::string field;
    while (ss >> field) fields.push_back(field);

    // utime=fields[11], stime=fields[12], rss=fields[21] (0-indexed after state)
    long utime = 0, stime = 0, rss_pages = 0;
    if (fields.size() > 22) {
        try {
            utime     = std::stol(fields[11]);
            stime     = std::stol(fields[12]);
            rss_pages = std::stol(fields[21]);
        } catch (...) {}
    }

    long hz     = sysconf(_SC_CLK_TCK);
    long pg_kb  = sysconf(_SC_PAGESIZE) / 1024;
    info.rss_kb = rss_pages * pg_kb;

    // Rough CPU%: (utime+stime) / (uptime * hz) * 100
    std::ifstream uptime_file("/proc/uptime");
    double uptime_s = 1.0;
    uptime_file >> uptime_s;
    double total_jiffies = uptime_s * hz;
    info.cpu_pct = total_jiffies > 0
        ? static_cast<float>((utime + stime) * 100.0 / total_jiffies)
        : 0.0f;

    return info;
}

static mcp::json process_list_handler(const mcp::json& params, const std::string&) {
    const std::string filter = params.value("filter", std::string{});
    const int         limit  = params.value("limit", 50);

    std::vector<ProcInfo> procs;

    for (const auto& entry : fs::directory_iterator("/proc")) {
        const std::string name = entry.path().filename().string();
        if (!std::all_of(name.begin(), name.end(), ::isdigit)) continue;
        auto info = read_proc_entry(entry.path());
        if (info.name.empty()) continue;
        if (!filter.empty() && info.name.find(filter) == std::string::npos) continue;
        procs.push_back(info);
    }

    // Sort by RSS descending
    std::sort(procs.begin(), procs.end(), [](const ProcInfo& a, const ProcInfo& b) {
        return a.rss_kb > b.rss_kb;
    });

    if (limit > 0 && static_cast<int>(procs.size()) > limit)
        procs.resize(static_cast<size_t>(limit));

    mcp::json list = mcp::json::array();
    for (const auto& p : procs) {
        list.push_back({
            {"pid",     p.pid},
            {"name",    p.name},
            {"state",   std::string(1, p.state)},
            {"rss_mb",  p.rss_kb / 1024},
            {"cpu_pct", p.cpu_pct}
        });
    }

    mcp::json result = {{"count", static_cast<int>(list.size())}, {"processes", list}};
    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

void register_process_tool(mcp::server& server) {
    auto tool = mcp::tool_builder("process_list")
        .with_description(
            "List running processes sorted by memory usage. "
            "Optionally filter by process name substring.")
        .with_string_param("filter", "Filter by process name (substring match)", false)
        .with_number_param("limit",  "Max processes to return (default: 50)",    false)
        .build();

    server.register_tool(tool, process_list_handler);
}
