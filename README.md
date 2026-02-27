# NeuralOS

**An agent-native OS layer for Linux.**

NeuralOS treats LLM agents as first-class system entities — named, supervised, and capable of calling OS primitives through a standard protocol — without requiring you to write any code to define or run them.

---

## The idea

Stock Linux wasn't designed for LLM agents. You get no lifecycle management, no tool access control, no standard interface between the model and the OS. NeuralOS adds that layer on top of an existing Linux system.

The mapping to OS concepts is direct:

| OS concept       | NeuralOS equivalent                              |
|------------------|--------------------------------------------------|
| Process          | `AgentInstance` — running LLM + tool loop        |
| init / systemd   | `nos-supervisor` — starts, stops, restarts agents|
| Scheduler        | `nos-supervisor` — per-agent mutex + priority    |
| Syscall          | MCP tool call (`exec`, `sysinfo`, `read_file` …) |
| /proc            | Agent registry (name, status, run count)         |
| Shell            | `nos` CLI — human-facing control interface       |

The protocol between agents and the OS is [MCP](https://modelcontextprotocol.io) (Model Context Protocol) — JSON-RPC 2.0 over HTTP/SSE. It's an open standard, which means any external MCP-compatible tool server can be plugged in without changing agent code.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                       nos  (CLI)                          │
└──────────────────────┬───────────────────────────────────┘
                       │ HTTP
┌──────────────────────▼───────────────────────────────────┐
│                  nos-supervisor                           │
│  Agent registry · Lifecycle · Request routing            │
└──────┬────────────────────────────────────┬──────────────┘
       │ creates                            │ calls tools via MCP
┌──────▼──────────┐               ┌────────▼──────────────┐
│  AgentInstance  │ ←── MCP ─────►│     nos-server         │
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

**`nos-server`** — MCP tool server. Exposes Linux OS primitives as callable tools. Runs standalone; agents connect to it over HTTP/SSE.

**`nos-supervisor`** — Agent lifecycle manager. Loads agent definitions from `agents/*.yaml`, creates `AgentInstance` objects on demand, routes requests, and exposes a REST API.

**`nos-agent-run`** — Low-level CLI to run a single agent directly (bypasses supervisor). Useful for development and debugging.

**`nos-builder`** — Asks the LLM to generate a new agent YAML from a natural-language description, writes it to `agents/`, and prints a reload command.

**`nos`** — The user-facing CLI. Wraps all the above into a single binary.

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
git clone --recurse-submodules https://github.com/your-org/NeuralOS.git
cd NeuralOS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binaries land in:

```
build/src/nos-server/nos-server
build/src/agent/nos-agent-run
build/src/nos-supervisor/nos-supervisor
build/src/nos-supervisor/nos-builder
build/src/nos-cli/nos
```

---

## Quick start

**1. Start the LLM backend** (example: llama-server from llama.cpp)

```bash
llama-server -m ~/models/gemma-3-4b-it-Q4_K_M.gguf --port 8080 -c 8192
```

Or point `NOS_LLM_URL` at a remote OpenAI-compatible API (Groq, etc.).

**2. Start nos-server**

```bash
./build/src/nos-server/nos-server
# Listening on localhost:8888 by default
```

**3. Start nos-supervisor**

```bash
./build/src/nos-supervisor/nos-supervisor --agents-dir ./agents
# Listening on localhost:9000 by default
```

**4. Use the CLI**

```bash
# List agents
./build/src/nos-cli/nos agents

# Ask the default agent a question
./build/src/nos-cli/nos ask "what processes are using the most memory?"

# Run a specific agent
./build/src/nos-cli/nos run sysmonitor "check disk usage and warn if any mount is above 80%"

# Check service health
./build/src/nos-cli/nos status
```

---

## Environment variables

| Variable          | Default                   | Description                              |
|-------------------|---------------------------|------------------------------------------|
| `NOS_LLM_URL`     | `http://localhost:8080`   | LLM backend base URL                     |
| `NOS_LLM_KEY`     | _(empty)_                 | API key for remote LLM backends          |
| `NOS_LLM_MODEL`   | _(server default)_        | Model name override                      |
| `NOS_SERVER_URL`  | `http://localhost:8888`   | nos-server URL (used by supervisor)      |
| `NOS_DEFAULT_AGENT` | `sysmonitor`            | Default agent for `nos ask`              |

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

The `tools` field is an allowlist — the agent can only call the tools listed, even if nos-server exposes more. A new agent is live after `nos reload` (no restart needed).

### Creating a new agent

```bash
# Describe what you want in natural language
./build/src/nos-supervisor/nos-builder "an agent that monitors nginx logs and summarizes errors"

# Reload so the supervisor picks it up
./build/src/nos-cli/nos reload
```

---

## Tests

```bash
# nos-server integration tests (requires nos-server on PATH or built)
bash tests/test_nos_server.sh

# nos-supervisor integration tests (starts its own supervisor instance)
bash tests/test_nos_supervisor.sh
```

---

## Project structure

```
NeuralOS/
├── agents/                  # Agent YAML definitions
│   ├── sysmonitor.yaml
│   ├── hello.yaml
│   └── builder.yaml
├── src/
│   ├── nos-server/          # MCP tool server
│   │   ├── main.cpp
│   │   └── tools/           # exec, filesystem, sysinfo, process, journal, network
│   ├── agent/               # AgentInstance + LLM client
│   │   ├── agent.h/cpp
│   │   ├── agent_config.h/cpp
│   │   ├── llm_client.h/cpp
│   │   └── agent_run.cpp    # nos-agent-run entry point
│   ├── nos-supervisor/      # Lifecycle manager + REST API
│   │   ├── supervisor.h/cpp
│   │   ├── main.cpp         # nos-supervisor entry point
│   │   └── builder_main.cpp # nos-builder entry point
│   └── nos-cli/             # nos CLI
│       └── main.cpp
├── tests/
│   ├── test_nos_server.sh
│   └── test_nos_supervisor.sh
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
