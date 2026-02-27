# AgentOS

**An agent-native OS layer for Linux.**

AgentOS treats LLM agents as first-class system entities — named, supervised, and capable of calling OS primitives through a standard protocol — without requiring you to write any code to define or run them.

---

## The idea

Stock Linux wasn't designed for LLM agents. You get no lifecycle management, no tool access control, no standard interface between the model and the OS. AgentOS adds that layer on top of an existing Linux system.

The mapping to OS concepts is direct:

| OS concept       | AgentOS equivalent                              |
|------------------|--------------------------------------------------|
| Process          | `AgentInstance` — running LLM + tool loop        |
| init / systemd   | `agentos-supervisor` — starts, stops, restarts agents|
| Scheduler        | `agentos-supervisor` — per-agent mutex + priority    |
| Syscall          | MCP tool call (`exec`, `sysinfo`, `read_file` …) |
| /proc            | Agent registry (name, status, run count)         |
| Shell            | `agentos` CLI — human-facing control interface       |

The protocol between agents and the OS is [MCP](https://modelcontextprotocol.io) (Model Context Protocol) — JSON-RPC 2.0 over HTTP/SSE. It's an open standard, which means any external MCP-compatible tool server can be plugged in without changing agent code.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    agentos  (CLI)                         │
└──────────────────────┬───────────────────────────────────┘
                       │ HTTP
┌──────────────────────▼───────────────────────────────────┐
│                  agentos-supervisor                           │
│  Agent registry · Lifecycle · Request routing            │
└──────┬────────────────────────────────────┬──────────────┘
       │ creates                            │ calls tools via MCP
┌──────▼──────────┐               ┌────────▼──────────────┐
│  AgentInstance  │ ←── MCP ─────►│     agentos-server         │
│  (per agent)    │               │  exec · read/write_file│
│  LLM + loop     │               │  sysinfo · process_list│
│  YAML config    │               │  journal_query         │
└─────────────────┘               │  network_info · list_dir│
                                  └────────────────────────┘
                                            │
                                  ┌─────────▼──────────────┐
                                  │   LLM backend           │
                                  │  llama-server (local)   │
                                  │  Groq / any OpenAI API  │
                                  └────────────────────────┘
```

---

## Components

**`agentos-server`** — MCP tool server. Exposes Linux OS primitives as callable tools. Runs standalone; agents connect to it over HTTP/SSE.

**`agentos-supervisor`** — Agent lifecycle manager. Loads agent definitions from `agents/*.yaml`, creates `AgentInstance` objects on demand, routes requests, and exposes a REST API.

**`agentos-agent-run`** — Low-level CLI to run a single agent directly (bypasses supervisor). Useful for development and debugging.

**`agentos-builder`** — Asks the LLM to generate a new agent YAML from a natural-language description, writes it to `agents/`, and prints a reload command.

**`agentos`** — The user-facing CLI. Wraps all the above into a single binary.

---

## Requirements

- Linux (kernel 5.10+)
- GCC or Clang with C++17
- CMake ≥ 3.14
- OpenSSL dev headers (`libssl-dev` on Debian/Ubuntu)
- `yaml-cpp` dev headers (`libyaml-cpp-dev`)
- An LLM backend with an OpenAI-compatible `/v1/chat/completions` endpoint

No other system dependencies — `cpp-mcp` (MCP library), `nlohmann/json`, and `cpp-httplib` are all vendored in `third-party/`.

---

## Build

```bash
git clone --recurse-submodules https://github.com/JulioAuto2ML/AgentOS.git
cd AgentOS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binaries land in:

```
build/src/agentos-server/agentos-server
build/src/agent/agentos-agent-run
build/src/agentos-supervisor/agentos-supervisor
build/src/agentos-supervisor/agentos-builder
build/src/agentos-cli/agentos
```

---

## Quick start

**1. Start the LLM backend** (example: llama-server from llama.cpp)

```bash
llama-server -m [YOUR MODEL].gguf --port 8080 -c 8192
```

Or point `AGENTOS_LLM_URL` at a remote OpenAI-compatible API (Groq, etc.).

**2. Start agentos-server**

```bash
./build/src/agentos-server/agentos-server
# Listening on localhost:8888 by default
```

**3. Start agentos-supervisor**

```bash
./build/src/agentos-supervisor/agentos-supervisor --agents-dir ./agents
# Listening on localhost:9000 by default
```

**4. Use the CLI**

```bash
# List agents
./build/src/agentos-cli/agentos agents

# Ask the default agent a question
./build/src/agentos-cli/agentos ask "what processes are using the most memory?"

# Run a specific agent
./build/src/agentos-cli/agentos run sysmonitor "check disk usage and warn if any mount is above 80%"

# Check service health
./build/src/agentos-cli/agentos status
```

---

## Environment variables

| Variable          | Default                   | Description                              |
|-------------------|---------------------------|------------------------------------------|
| `AGENTOS_LLM_URL`     | `http://localhost:8080`   | LLM backend base URL                     |
| `AGENTOS_LLM_KEY`     | _(empty)_                 | API key for remote LLM backends          |
| `AGENTOS_LLM_MODEL`   | _(server default)_        | Model name override                      |
| `AGENTOS_SERVER_URL`  | `http://localhost:8888`   | agentos-server URL (used by supervisor)      |
| `AGENTOS_DEFAULT_AGENT` | `sysmonitor`            | Default agent for `agentos ask`              |

---

## Agent definitions

Agents live in `agents/*.yaml`. Example:

```yaml
name: sysmonitor
description: Monitors system health and reports anomalies
model: default
tools: [sysinfo, process_list, journal_query, exec]
priority: normal
context_limit: 4096
max_steps: 10
system_prompt: |
  You are a system monitoring agent. Check system health and report
  issues concisely. Use sysinfo for resource usage, process_list to
  find memory hogs, and journal_query to check for errors.
```

The `tools` field is an allowlist — the agent can only call the tools listed, even if agentos-server exposes more. A new agent is live after `agentos reload` (no restart needed).

### Creating a new agent

```bash
# Describe what you want in natural language
./build/src/agentos-supervisor/agentos-builder "an agent that monitors nginx logs and summarizes errors"

# Reload so the supervisor picks it up
./build/src/agentos-cli/agentos reload
```

---

## Tests

```bash
# agentos-server integration tests (requires agentos-server on PATH or built)
bash tests/test_agentos_server.sh

# agentos-supervisor integration tests (starts its own supervisor instance)
bash tests/test_agentos_supervisor.sh
```

---

## Project structure

```
AgentOS/
├── agents/                  # Agent YAML definitions
│   ├── sysmonitor.yaml
│   ├── hello.yaml
│   └── builder.yaml
├── src/
│   ├── agentos-server/          # MCP tool server
│   │   ├── main.cpp
│   │   └── tools/           # exec, filesystem, sysinfo, process, journal, network
│   ├── agent/               # AgentInstance + LLM client
│   │   ├── agent.h/cpp
│   │   ├── agent_config.h/cpp
│   │   ├── llm_client.h/cpp
│   │   └── agent_run.cpp    # agentos-agent-run entry point
│   ├── agentos-supervisor/      # Lifecycle manager + REST API
│   │   ├── supervisor.h/cpp
│   │   ├── main.cpp         # agentos-supervisor entry point
│   │   └── builder_main.cpp # agentos-builder entry point
│   └── agentos-cli/             # agentos CLI
│       └── main.cpp
├── tests/
│   ├── test_agentos_server.sh
│   └── test_agentos_supervisor.sh
├── third-party/
│   └── cpp-mcp/             # Vendored MCP library (MIT)
├── CMakeLists.txt
└── optimized-dev-plan.md
```

---

## Roadmap

Phases 1–5 are complete (v0.1). Planned next steps:

- **Persistent agent memory** — context storage between runs
- **Supervisor authentication** — token-based auth on the HTTP API
- **Parallel agent execution** — concurrent runs of different agents
- **Additional channels** — Telegram bot, HTTP webhook ingestion
- **sched_ext integration** — Linux 6.12+ eBPF scheduler hooks for LLM-aware CPU scheduling

---

## License

See [LICENSE](LICENSE).
