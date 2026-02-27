// =============================================================================
// tools/network.cpp — MCP tool: network_info
// =============================================================================
//
// Reports network interface state, addresses, and traffic counters.
//
// Data sources (all kernel-exported, no root required):
//
//   /sys/class/net/<iface>/operstate  — "up", "down", "unknown"
//   /sys/class/net/<iface>/address    — MAC address (xx:xx:xx:xx:xx:xx)
//   /sys/class/net/<iface>/mtu        — MTU in bytes
//
//   /proc/net/dev  — Cumulative RX/TX byte counters per interface.
//                   Format: two header lines, then one line per interface:
//                   "  eth0:  rx_bytes rx_pkts ... tx_bytes tx_pkts ..."
//                   We parse fields 0 (rx_bytes) and 8 (tx_bytes) after iface.
//
//   ip addr show   — IPv4 address with prefix (e.g. 192.168.1.10/24).
//                   We call `ip` rather than parsing /proc/net/fib_trie, which
//                   is complex and poorly documented.
//
// Interfaces can be filtered by name via the `interface` parameter.
// Loopback (lo) is included unless filtered out — useful for agents that need
// to verify that localhost services are bound.
// =============================================================================

#include "tools.h"
#include "mcp_message.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// Read a single-line value from a sysfs file, trimmed.
static std::string sysfs_read(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::string v;
    std::getline(f, v);
    // trim
    v.erase(v.find_last_not_of(" \t\r\n") + 1);
    return v;
}

// Parse /proc/net/dev for TX/RX byte counters per interface.
static std::map<std::string, std::pair<long long, long long>> read_net_dev() {
    std::map<std::string, std::pair<long long, long long>> stats;
    std::ifstream f("/proc/net/dev");
    std::string line;
    std::getline(f, line); // header 1
    std::getline(f, line); // header 2
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string iface;
        ss >> iface;
        if (!iface.empty() && iface.back() == ':') iface.pop_back();
        long long rx_bytes, dummy;
        ss >> rx_bytes;                        // rx_bytes
        for (int i = 0; i < 7; i++) ss >> dummy; // rx_packets…
        long long tx_bytes;
        ss >> tx_bytes;                        // tx_bytes
        stats[iface] = {rx_bytes, tx_bytes};
    }
    return stats;
}

static mcp::json network_handler(const mcp::json& params, const std::string&) {
    const std::string filter = params.value("interface", std::string{});

    auto net_stats = read_net_dev();

    mcp::json interfaces = mcp::json::array();

    const fs::path net_base = "/sys/class/net";
    if (!fs::exists(net_base)) {
        return mcp::json::array({{{"type","text"},{"text","Cannot read /sys/class/net"}}});
    }

    for (const auto& entry : fs::directory_iterator(net_base)) {
        const std::string iface = entry.path().filename().string();
        if (!filter.empty() && iface != filter) continue;

        std::string operstate = sysfs_read(entry.path() / "operstate"); // up/down/unknown
        std::string address   = sysfs_read(entry.path() / "address");   // MAC
        std::string mtu       = sysfs_read(entry.path() / "mtu");

        // Collect IPv4 addresses via /proc/net/fib_trie or fall back to `ip addr`
        // For simplicity use ip addr output (always available on modern Linux)
        std::string addr_cmd = "ip -4 addr show " + iface + " 2>/dev/null | grep 'inet ' | awk '{print $2}'";
        FILE* pipe = popen(addr_cmd.c_str(), "r");
        std::string ipv4;
        if (pipe) {
            char buf[64]{};
            if (fgets(buf, sizeof(buf), pipe)) {
                ipv4 = buf;
                ipv4.erase(ipv4.find_last_not_of(" \t\r\n") + 1);
            }
            pclose(pipe);
        }

        long long rx_bytes = 0, tx_bytes = 0;
        if (net_stats.count(iface)) {
            rx_bytes = net_stats[iface].first;
            tx_bytes = net_stats[iface].second;
        }

        interfaces.push_back({
            {"interface",  iface},
            {"state",      operstate},
            {"mac",        address},
            {"ipv4",       ipv4},
            {"mtu",        mtu},
            {"rx_bytes",   rx_bytes},
            {"tx_bytes",   tx_bytes}
        });
    }

    mcp::json result = {{"interfaces", interfaces}};
    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

void register_network_tool(mcp::server& server) {
    auto tool = mcp::tool_builder("network_info")
        .with_description(
            "List network interfaces with their state, IP, MAC, MTU, "
            "and cumulative RX/TX byte counters.")
        .with_string_param("interface", "Filter to a specific interface name (optional)", false)
        .build();

    server.register_tool(tool, network_handler);
}
