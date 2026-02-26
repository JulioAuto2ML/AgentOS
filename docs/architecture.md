# NeuralOS — Architecture Overview

## What is NeuralOS?

NeuralOS is a Linux-based system designed to make **LLM agents first-class OS citizens**. Rather than running agents as ad-hoc scripts, NeuralOS treats them like processes: named, isolated, prioritised, and supervised. The goal is to provide what stock Linux doesn't: lifecycle management, tool access control, context budgets, and easy agent creation — all without requiring the user to write code.

## The Core Insight

"LLM at the kernel level" doesn't mean moving the model into the kernel (that's impossible — inference takes seconds, kernel decisions take microseconds). It means applying OS-level thinking to agent management:

| OS concept       | NeuralOS equivalent                        |
|------------------|--------------------------------------------|
| Process          | AgentInstance (running LLM + tool loop)    |
| Scheduler        | nos-supervisor (priority + resource quotas)|
| Syscall          | MCP tool call (exec, read_file, sysinfo…) |
| /proc filesystem | Agent registry (name, status, context)     |
| init/systemd     | nos-supervisor (start, stop, restart)      |
| Shell            | nos-cli (user-facing control interface)    |

## Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                      User / CLI                          │
│                     (nos-cli)                            │
└────────────────────────┬────────────────────────────────┘
                         │ UNIX socket / HTTP
┌────────────────────────▼────────────────────────────────┐
│                   nos-supervisor                         │
│  • Agent registry        • Lifecycle (start/stop/restart)│
│  • Request routing       • Priority scheduling           │
│  • Builder agent         • Context budget enforcement    │
└────┬──────────────────────────────────────┬─────────────┘
     │ spawns                                │ calls
┌────▼──────────┐                  ┌────────▼────────────┐
│  AgentInstance│                  │     nos-server       │
│  (per agent)  │  ←── MCP ──────► │  (MCP tool server)  │
│               │                  │                      │
│  • LLM client │                  │  Tools:              │
│  • Tool loop  │                  │  • exec              │
│  • Context    │                  │  • read/write_file   │
│  • YAML config│                  │  • list_dir          │
└───────────────┘                  │  • sysinfo           │
                                   │  • process_list      │
                                   │  • journal_query     │
                                   │  • network_info      │
                                   └─────────────────────┘
                                             │
                                   ┌─────────▼────────────┐
                                   │    LLM Backend        │
                                   │  (pluggable)          │
                                   │  • llama-server local │
                                   │  • Groq API           │
                                   │  • Anthropic API      │
                                   │  • any OpenAI-compat  │
                                   └──────────────────────┘
```

## Protocol: MCP (Model Context Protocol)

All tool calls go through MCP — JSON-RPC 2.0 over HTTP/SSE. This means:
- nos-server is **LLM-agnostic**: any model that can make HTTP calls can use its tools.
- Agent isolation is enforced by **tool allowlists** in the agent YAML.
- nos-server can run on a different machine from the agents (useful for distributed setups).

The MCP wire format for a tool call looks like:

```json
// Request (agent → nos-server)
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "sysinfo",
    "arguments": {}
  }
}

// Response (nos-server → agent)
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{"type": "text", "text": "{\"cpu\":{...},\"memory\":{...}}"}]
  }
}
```

## Agent Definition (YAML)

Agents are defined in YAML files under `agents/`. Example:

```yaml
name: sysmonitor
description: Monitors system health and alerts on anomalies
model: default                  # uses supervisor's default LLM
tools: [sysinfo, process_list, journal_query, exec]
priority: normal
context_limit: 4096
max_steps: 10
system_prompt: |
  You are a system monitoring agent for NeuralOS. Your job is to
  check system health and report issues concisely. Use sysinfo to
  get resource usage, process_list to find memory hogs, and
  journal_query to check for errors.
```

The `tools` list is an allowlist — the agent can only call the listed tools, even if nos-server exposes more.

## Data Flow: One Agent Turn

```
1. User request arrives at nos-supervisor
2. Supervisor selects/creates AgentInstance
3. AgentInstance sends prompt to LLM backend
4. LLM responds with a tool call (e.g. sysinfo)
5. AgentInstance sends MCP request to nos-server
6. nos-server executes tool, returns JSON result
7. AgentInstance appends result to context, calls LLM again
8. Repeat steps 4–7 up to max_steps times
9. LLM produces final text response (no tool call)
10. AgentInstance returns response to supervisor → user
```

## Development Phases

| Phase | Component         | Status      |
|-------|-------------------|-------------|
| 1     | nos-server        | ✅ Complete  |
| 2     | Agent Model + LLM | 🔧 In progress |
| 3     | nos-supervisor v1 | ⏳ Pending  |
| 4     | Builder Agent     | ⏳ Pending  |
| 5     | nos-cli           | ⏳ Pending  |
| 6     | Kernel integration (sched_ext) | 🔬 Research |

## Hardware Context

Development hardware is all CPU-only (no discrete GPU). See `hardware.md` for specs.

LLM strategy:
- **Local**: `llama-server` from llama.cpp (`/home/julio/llama.cpp/bin/llama-server`)
- **Remote dev/test**: Groq free tier (~300 tok/s on Llama 3.1 70B)
- **Benchmarks**: RunPod on-demand RTX 3090 (~$0.20/hr)
