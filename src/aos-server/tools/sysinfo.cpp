// =============================================================================
// tools/sysinfo.cpp — MCP tool: sysinfo
// =============================================================================
//
// Returns a JSON snapshot of system resources. Data sources:
//
//   CPU usage  — /proc/stat, sampled twice 100 ms apart.
//                Formula: (busy_delta / total_delta) * 100.
//                "busy" = user + nice + system + irq + softirq + steal.
//                "total" = busy + idle + iowait.
//                The 100 ms sleep gives a real-time sample rather than a
//                lifetime average — much more useful for agents monitoring load.
//
//   Memory     — sys/sysinfo.h sysinfo(2) syscall.
//                Gives totalram, freeram in bytes via mem_unit multiplier.
//
//   Disk       — statvfs(2) on "/" for f_blocks, f_bfree, f_frsize.
//                Only the root filesystem is reported; agents can call
//                exec("df -h") for a full picture.
//
//   Uptime     — sysinfo.uptime (seconds since boot).
//
//   Load avg   — sysinfo.loads[0/1/2] stored as fixed-point / 65536.
//                Matches the three values shown by `uptime`.
//
//   Process count — sysinfo.procs (total runnable + sleeping).
// =============================================================================

#include "tools.h"
#include "mcp_message.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string read_proc(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Returns (user+system) jiffies and total jiffies from /proc/stat for cpu0+
static std::pair<long long, long long> read_cpu_jiffies() {
    std::ifstream f("/proc/stat");
    std::string line;
    std::getline(f, line); // first line: cpu aggregate
    std::istringstream ss(line);
    std::string tag;
    ss >> tag; // "cpu"
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    long long busy  = user + nice + system + irq + softirq + steal;
    long long total = busy + idle + iowait;
    return {busy, total};
}

// Sample CPU usage over ~100ms
static double cpu_usage_percent() {
    auto [b1, t1] = read_cpu_jiffies();
    usleep(100000); // 100 ms
    auto [b2, t2] = read_cpu_jiffies();
    long long db = b2 - b1, dt = t2 - t1;
    if (dt == 0) return 0.0;
    return 100.0 * static_cast<double>(db) / static_cast<double>(dt);
}

static mcp::json disk_usage(const std::string& mount = "/") {
    struct statvfs vfs{};
    statvfs(mount.c_str(), &vfs);
    long long total = static_cast<long long>(vfs.f_blocks) * vfs.f_frsize;
    long long free_ = static_cast<long long>(vfs.f_bfree)  * vfs.f_frsize;
    long long used  = total - free_;
    return {
        {"mount",      mount},
        {"total_mb",   total / (1024*1024)},
        {"used_mb",    used  / (1024*1024)},
        {"free_mb",    free_ / (1024*1024)},
        {"used_pct",   total > 0 ? 100.0 * used / total : 0.0}
    };
}

// ── handler ───────────────────────────────────────────────────────────────────

static mcp::json sysinfo_handler(const mcp::json& /*params*/, const std::string&) {
    struct sysinfo si{};
    sysinfo(&si);

    long long mem_total_mb = static_cast<long long>(si.totalram)  * si.mem_unit / (1024*1024);
    long long mem_free_mb  = static_cast<long long>(si.freeram)   * si.mem_unit / (1024*1024);
    long long mem_used_mb  = mem_total_mb - mem_free_mb;

    // Load average (1, 5, 15 min) — sysinfo stores as fixed-point / 65536
    double load1  = si.loads[0] / 65536.0;
    double load5  = si.loads[1] / 65536.0;
    double load15 = si.loads[2] / 65536.0;

    long uptime_s = si.uptime;
    long uptime_h = uptime_s / 3600;
    long uptime_m = (uptime_s % 3600) / 60;

    int nprocs = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

    mcp::json result = {
        {"cpu", {
            {"cores_online",  nprocs},
            {"usage_pct",     cpu_usage_percent()}
        }},
        {"memory", {
            {"total_mb", mem_total_mb},
            {"used_mb",  mem_used_mb},
            {"free_mb",  mem_free_mb},
            {"used_pct", mem_total_mb > 0 ? 100.0 * mem_used_mb / mem_total_mb : 0.0}
        }},
        {"disk",   disk_usage("/")},
        {"uptime", {
            {"seconds", uptime_s},
            {"human",   std::to_string(uptime_h) + "h " + std::to_string(uptime_m) + "m"}
        }},
        {"load_avg", {
            {"1min",  load1},
            {"5min",  load5},
            {"15min", load15}
        }},
        {"processes", {
            {"total", static_cast<int>(si.procs)}
        }}
    };

    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

void register_sysinfo_tool(mcp::server& server) {
    auto tool = mcp::tool_builder("sysinfo")
        .with_description(
            "Return a snapshot of system resources: CPU usage, memory, disk, "
            "uptime, load average, and process count.")
        .build();

    server.register_tool(tool, sysinfo_handler);
}
