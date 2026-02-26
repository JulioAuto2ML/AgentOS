# nos-server — Reference Manual

nos-server is the MCP tool server for NeuralOS. It exposes Linux OS primitives
as callable tools that LLM agents can invoke via JSON-RPC 2.0 over HTTP/SSE.

## Build

```bash
cd ~/Documents/GitHub/NeuralOS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target nos-server -j$(nproc)
# Binary: ./build/src/nos-server/nos-server
```

Requirements: GCC or Clang with C++17, CMake ≥ 3.14, pthreads. No other
system libraries needed — cpp-mcp is vendored in `third-party/`.

## Run

```bash
# Defaults: localhost:8888
./build/src/nos-server/nos-server

# Custom host/port
./build/src/nos-server/nos-server --host 0.0.0.0 --port 9000
```

On startup, the server prints the registered tools and starts listening.
Stop with Ctrl-C (SIGINT) or `kill <pid>` (SIGTERM) for graceful shutdown.

## Test

```bash
cd ~/Documents/GitHub/NeuralOS
bash tests/test_nos_server.sh
```

The test script starts nos-server on port 18888, calls each tool via `curl`,
verifies the response structure, and prints live data from the machine.

Expected output (all green):

```
════════════════════════════════════════
  NeuralOS nos-server — Tests Fase 1
════════════════════════════════════════

[INFO] Test 1: sysinfo
[PASS] sysinfo → contiene cpu, memory, disk
   CPU: 12.3% (6 cores)
   RAM: 4821MB usados de 15258MB (31.6%)
   Disco: 28340MB usados de 468886MB (6.0%)
   Uptime: 5h 30m
...
════════════════════════════════════════
Todos los tests pasaron ✓
════════════════════════════════════════
```

## MCP Protocol

nos-server implements [MCP](https://modelcontextprotocol.io) over HTTP/SSE.

A minimal tool call with `curl`:

```bash
# Start a session (SSE endpoint — keep this open in a separate terminal)
curl -N http://localhost:8888/sse?session_id=test

# Call a tool
curl -s -X POST http://localhost:8888/message?session_id=test \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {"name": "sysinfo", "arguments": {}}
  }'
```

The result arrives on the SSE stream as a `message` event with a JSON payload.
The test script's `call_tool()` function handles this transparently.

## Tool Reference

### `sysinfo`

Returns a snapshot of system resources.

**Parameters:** none

**Returns:**
```json
{
  "cpu":       {"cores_online": 6, "usage_pct": 12.3},
  "memory":    {"total_mb": 15258, "used_mb": 4821, "free_mb": 10437, "used_pct": 31.6},
  "disk":      {"mount": "/", "total_mb": 468886, "used_mb": 28340, "free_mb": 416490, "used_pct": 6.0},
  "uptime":    {"seconds": 19800, "human": "5h 30m"},
  "load_avg":  {"1min": 0.45, "5min": 0.38, "15min": 0.31},
  "processes": {"total": 312}
}
```

CPU usage is sampled over 100 ms for a real-time reading (not a lifetime average).

---

### `exec`

Run a shell command and return its output.

**Parameters:**
| Name          | Type   | Required | Description |
|---------------|--------|----------|-------------|
| `command`     | string | yes      | Shell command to run via `/bin/sh -c` |
| `timeout_ms`  | number | no       | Timeout in milliseconds (default: 10000) |
| `working_dir` | string | no       | Working directory (default: inherited) |

**Returns:**
```json
{
  "stdout":    "hello NeuralOS\n",
  "exit_code": 0,
  "timed_out": false
}
```

stdout and stderr are merged. If the command times out, `exit_code` is 124
and `timed_out` is true.

---

### `read_file`

Read the contents of a file.

**Parameters:**
| Name     | Type   | Required | Description |
|----------|--------|----------|-------------|
| `path`   | string | yes      | Absolute or relative path |
| `offset` | number | no       | Byte offset to start reading from (default: 0) |
| `limit`  | number | no       | Max bytes to read (default: 0 = all) |

**Returns:**
```json
{
  "path":    "/proc/version",
  "content": "Linux version 6.8.0-57-generic ...",
  "size":    150
}
```

---

### `write_file`

Write or append content to a file. Creates parent directories as needed.

**Parameters:**
| Name      | Type    | Required | Description |
|-----------|---------|----------|-------------|
| `path`    | string  | yes      | Path to write |
| `content` | string  | yes      | Text to write |
| `append`  | boolean | no       | Append instead of overwrite (default: false) |

**Returns:**
```json
{
  "path":          "/tmp/output.txt",
  "bytes_written": 42,
  "appended":      false
}
```

---

### `list_dir`

List directory entries.

**Parameters:**
| Name   | Type   | Required | Description |
|--------|--------|----------|-------------|
| `path` | string | yes      | Path to the directory |

**Returns:**
```json
{
  "path": "/tmp",
  "entries": [
    {"name": "nos_test_1234.txt", "type": "file",    "size": 42},
    {"name": "some_dir",          "type": "dir",     "size": -1},
    {"name": "link_to_something", "type": "symlink", "size": -1}
  ]
}
```

---

### `process_list`

List running processes sorted by memory usage.

**Parameters:**
| Name     | Type   | Required | Description |
|----------|--------|----------|-------------|
| `filter` | string | no       | Substring match on process name |
| `limit`  | number | no       | Max processes to return (default: 50) |

**Returns:**
```json
{
  "count": 5,
  "processes": [
    {"pid": 1234, "name": "firefox",  "state": "S", "rss_mb": 512, "cpu_pct": 3.2},
    {"pid": 5678, "name": "llama-sv", "state": "R", "rss_mb": 480, "cpu_pct": 95.1}
  ]
}
```

State codes: `R`=running, `S`=sleeping, `D`=disk wait, `Z`=zombie, `T`=stopped.
CPU% is a lifetime average (utime+stime / uptime). Use `sysinfo` for real-time CPU.

---

### `journal_query`

Query system logs via journalctl (falls back to `/var/log/syslog`).

**Parameters:**
| Name    | Type   | Required | Description |
|---------|--------|----------|-------------|
| `unit`  | string | no       | Systemd unit name, e.g. `nginx` |
| `lines` | number | no       | Lines to return (default: 50) |
| `since` | string | no       | Time range, e.g. `"1 hour ago"`, `"2025-01-01"` |
| `level` | string | no       | Log level: `err`, `warning`, `info`, `debug` |

**Returns:**
```json
{
  "lines":  50,
  "unit":   "nginx",
  "since":  "1 hour ago",
  "output": "Jan 01 12:00:00 hostname nginx[1234]: ..."
}
```

---

### `network_info`

List network interfaces with state, addresses, and traffic counters.

**Parameters:**
| Name        | Type   | Required | Description |
|-------------|--------|----------|-------------|
| `interface` | string | no       | Filter to a specific interface (e.g. `eth0`) |

**Returns:**
```json
{
  "interfaces": [
    {
      "interface": "eth0",
      "state":     "up",
      "mac":       "aa:bb:cc:dd:ee:ff",
      "ipv4":      "192.168.1.10/24",
      "mtu":       "1500",
      "rx_bytes":  1234567890,
      "tx_bytes":  987654321
    }
  ]
}
```

## Source Layout

```
src/nos-server/
├── main.cpp            # Entry point: arg parsing, tool registration, server start
└── tools/
    ├── tools.h         # Registration function declarations
    ├── exec.cpp        # exec tool
    ├── filesystem.cpp  # read_file, write_file, list_dir tools
    ├── sysinfo.cpp     # sysinfo tool
    ├── process.cpp     # process_list tool
    ├── journal.cpp     # journal_query tool
    └── network.cpp     # network_info tool

third-party/
└── cpp-mcp/            # Vendored MCP library (MIT, github.com/hkr04/cpp-mcp)
    ├── include/        # mcp_server.h, mcp_tool.h, mcp_message.h, …
    ├── src/            # mcp_server.cpp, mcp_tool.cpp, …
    └── common/         # json.hpp (nlohmann), httplib.h, base64.hpp

tests/
└── test_nos_server.sh  # Integration test script
```

## Adding a New Tool

1. Create `src/nos-server/tools/mytool.cpp`:

```cpp
// =============================================================================
// tools/mytool.cpp — MCP tool: my_tool
// =============================================================================
// Brief description of what this tool does and why.

#include "tools.h"
#include "mcp_message.h"

static mcp::json my_tool_handler(const mcp::json& params, const std::string&) {
    // Validate params
    if (!params.contains("my_param"))
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'my_param'");

    // Do the work
    std::string value = params["my_param"].get<std::string>();

    // Return result
    mcp::json result = {{"my_result", value}};
    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

void register_my_tool(mcp::server& server) {
    auto tool = mcp::tool_builder("my_tool")
        .with_description("Does something useful for agents.")
        .with_string_param("my_param", "Description of my_param")
        .build();
    server.register_tool(tool, my_tool_handler);
}
```

2. Add `tools/mytool.cpp` to `src/nos-server/CMakeLists.txt`.
3. Declare `void register_my_tool(mcp::server&);` in `tools/tools.h`.
4. Call `register_my_tool(server);` in `main.cpp`.
5. Add a test case to `tests/test_nos_server.sh`.
