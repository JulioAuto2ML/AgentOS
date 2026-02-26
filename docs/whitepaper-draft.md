# NeuralOS: An Agent-Native Operating System Layer for Linux

**Draft — February 2026**

---

## Abstract

Current approaches to deploying large language model (LLM) agents treat them as application-level constructs: objects instantiated by framework libraries, invisible to the operating system, unmanaged by the kernel scheduler, and without lifecycle or resource guarantees. This paper argues that this architectural choice is the root cause of the reliability, scalability, and composability problems consistently reported by agent developers. We propose NeuralOS, an agent-native OS layer for Linux that treats LLM agents as first-class system entities with the same properties that processes have had since the 1970s: named existence, supervised lifecycle, priority-based scheduling, capability-controlled access to system resources, and a standard protocol for interacting with the rest of the system. NeuralOS builds on the Model Context Protocol (MCP) as the agent-system interface, llama.cpp and any OpenAI-compatible API as the pluggable inference backend, and Linux's sched_ext framework as the path toward kernel-level agent scheduling. We describe the architecture, implementation, and the design space it opens.

---

## 1. Introduction

The deployment of LLM-based agents has grown rapidly since 2023, but the infrastructure supporting them has not kept pace. The dominant paradigm — application-level frameworks that instantiate agents as Python objects — has proven adequate for prototypes and simple pipelines but consistently fails in production scenarios requiring reliability, concurrent execution, resource control, and long-running operation.

The failure mode is architectural, not incidental. Application-level frameworks provide agents with no durable identity, no lifecycle guarantees, no resource quotas, no scheduling semantics, and no standard interface to system resources. These are not missing features that can be added to an existing framework; they are properties of the *layer at which agents run*, and that layer is currently the wrong one.

This paper proposes a different layer: the operating system.

The motivation is straightforward. Every problem that plagues agent frameworks in production — unbounded resource consumption, no restart on failure, no concurrency management, no standard tool interface, no human-readable management surface — is a problem that operating systems solved for processes decades ago. The solution is not to re-solve it inside a framework but to recognize agents as processes and manage them accordingly.

We present NeuralOS, an open-source implementation of this idea as a C++ daemon stack running on Linux. NeuralOS does not modify the kernel or replace any existing OS component. It adds a layer between user-space agents and the Linux kernel that provides the missing OS-level abstractions.

The contribution of this paper is threefold:

1. A formal analysis of the structural gap between current agent frameworks and OS-level requirements.
2. An architecture that maps classical OS abstractions (process, scheduler, syscall, supervisor) to their agent equivalents.
3. A working implementation demonstrating feasibility on commodity CPU-only hardware.

---

## 2. Background and Related Work

### 2.1 The LLM OS Concept

The conceptual foundation for treating LLMs as OS components was articulated publicly by Andrej Karpathy in late 2023. Karpathy proposed viewing a large language model not as a chatbot but as "the kernel process of a new Operating System" — a central computational unit that orchestrates input/output, manages context (analogous to RAM), accesses persistent storage (via embeddings), and executes programs (via code interpreters). His framing established the vocabulary: LLM as CPU, context window as RAM, embedding stores as filesystem, tool calls as system calls.

This framing is productive but deliberately analogical. Karpathy described what the *logical* architecture should look like; he did not describe how to build it on real hardware.

### 2.2 AIOS: LLM Agent Operating System

The most significant academic contribution to this space is AIOS [1], published at COLM 2025 by Mei et al. from Rutgers University. AIOS proposes a dedicated kernel for LLM-based agents, implemented in Python and providing:

- **Agent scheduler**: Manages concurrent agent execution
- **Context manager**: Saves and restores LLM generation state, enabling context switching between agents
- **Memory manager**: Short-term (context window) and long-term (persistent, cross-session) per-agent memory
- **Storage manager**: Persistent data access across sessions
- **Access control**: Resource permission management

AIOS reports up to 2.1x performance improvement over traditional agent frameworks, attributed primarily to better resource utilization under concurrent agent workloads.

NeuralOS differs from AIOS in several important dimensions. AIOS is implemented in Python and targets the agent framework layer; NeuralOS is implemented in C++ and targets the OS daemon layer. AIOS introduces a custom memory model; NeuralOS defers memory management to the LLM backend and relies on OS-level process isolation. AIOS does not expose a standard protocol; NeuralOS uses MCP as its tool interface. Most importantly, AIOS does not interact with the Linux kernel scheduler; NeuralOS's roadmap includes sched_ext integration for kernel-aware agent scheduling.

The two projects address the same problem from different architectural positions and are complementary rather than competing.

### 2.3 Towards Agentic OS: AI-Driven Kernel Scheduling

Zheng et al. (2025) [2] demonstrate a more radical integration in SchedCP: LLM agents that autonomously optimize Linux kernel schedulers via eBPF programs. The control plane exposes scheduling primitives as MCP tools; a multi-agent system (observation, planning, execution, learning agents) operates within this interface. Results include a 1.79x workload performance improvement and a 13x cost reduction versus naive LLM-based approaches.

SchedCP establishes two important points for NeuralOS: (a) MCP is an appropriate protocol for exposing kernel-level primitives to agents, and (b) kernel-level integration is feasible and valuable. NeuralOS's planned sched_ext integration follows the path SchedCP demonstrates.

### 2.4 Agent.xpu: Heterogeneous Workload Scheduling

Chen et al. (2025) [3] address the hardware scheduling problem for agentic workloads. Agent.xpu orchestrates reactive (low-latency, user-initiated) and proactive (high-throughput, background) LLM workloads on heterogeneous SoCs — CPU, integrated GPU, and NPU — using operator-accelerator affinity mapping and kernel-level preemption. The work demonstrates that running multiple agent workloads concurrently requires hardware-aware scheduling that no general-purpose OS provides today.

NeuralOS's priority system (realtime > high > normal > low) is a first approximation of the semantic scheduling Agent.xpu formalizes at the hardware level.

### 2.5 Model Context Protocol

The Model Context Protocol (MCP) [4], released by Anthropic in November 2024 and donated to the Agentic AI Foundation (a Linux Foundation directed fund) in December 2025, defines a JSON-RPC 2.0 over HTTP/SSE protocol for communication between AI clients and tool servers. As of December 2025, MCP has 97 million monthly SDK downloads and 17,000+ community-built servers. It is supported natively by Claude, ChatGPT, Gemini, Cursor, and Visual Studio Code.

MCP solves the tool interface standardization problem that previously required each agent framework to define its own tool calling semantics. For NeuralOS, MCP serves as the standard protocol for the agent-OS interface: agents call system tools the same way they call any other MCP tool, and any MCP-compatible tool server (regardless of origin) is immediately usable by NeuralOS agents.

### 2.6 Application-Level Framework Limitations

LangChain's 2024 State of AI Agents report [5] documents the limitations of the dominant framework approach from the perspective of practitioners: the primary limitation is "performance quality," the hardest problem is context management, and frameworks create abstraction ceilings that prevent production deployments of non-trivial agents. CrewAI, despite 150+ enterprise customers and 100,000+ daily agent executions, still operates at the application layer without OS-level resource management. AutoGPT's architectural analysis reveals no scheduling semantics, no lifecycle guarantees, and no standard system interface.

These limitations are not bugs in the frameworks; they are consequences of the layer at which the frameworks operate.

---

## 3. Architectural Principles

NeuralOS is guided by four principles derived from classical OS design.

**P1 — Named entity principle.** An agent must have a stable, durable identity independent of any particular execution. This is the difference between a process (which exists in the process table, has a PID, can be listed with `ps`) and a function call (which exists only during its stack frame). NeuralOS agents have names, appear in the agent registry, and persist across individual requests.

**P2 — Supervised lifecycle principle.** An agent's lifecycle (creation, execution, failure, recovery) must be managed by a supervisor that can make policy decisions. Agents should not self-terminate on failure; the supervisor should catch failures, log them, and decide whether to restart. This is the Unix init model applied to agents.

**P3 — Capability-controlled resource access principle.** Agents must access system resources only through a controlled interface that enforces a capability model. An agent with `tools: [sysinfo, exec]` can read system metrics and run commands; it cannot access the filesystem or network unless those tools are explicitly granted. This is the principle of least privilege, implemented as a configuration-time allowlist.

**P4 — Protocol uniformity principle.** The interface between agents and resources must be a single, standard protocol, not a collection of framework-specific adapters. Any resource (filesystem, database, external service, kernel primitive) exposed via MCP is immediately available to any NeuralOS agent without modification.

---

## 4. Architecture

### 4.1 System Overview

NeuralOS consists of five components organized in a layered architecture:

```
┌──────────────────────────────────────────────────────┐
│                    User Interface                     │
│                  nos CLI / REST API                   │
└───────────────────────┬──────────────────────────────┘
                        │
┌───────────────────────▼──────────────────────────────┐
│                  nos-supervisor                       │
│  Agent Registry  │  Priority Queue  │  HTTP API      │
└──────┬───────────────────────────────────┬────────────┘
       │ spawns / manages                  │ dispatches
┌──────▼──────────┐              ┌─────────▼──────────┐
│  AgentInstance  │◄── MCP ─────►│    nos-server       │
│  LLM client     │              │  (MCP Tool Server)  │
│  Tool loop      │              │                     │
│  Context budget │              │  Tools:             │
└─────────────────┘              │  exec, read_file,   │
                                 │  write_file,        │
                                 │  list_dir, sysinfo, │
                                 │  process_list,      │
                                 │  journal_query,     │
                ┌────────────────┘  network_info       │
                │                └────────────────────┘
┌───────────────▼──────────────────────────────────────┐
│               LLM Backend (pluggable)                 │
│  llama-server (local) │ Groq │ Anthropic │ any OAIC  │
└──────────────────────────────────────────────────────┘
```

### 4.2 nos-server: The System Call Layer

`nos-server` is an MCP tool server implemented in C++ using a vendored copy of cpp-mcp (MIT license). It runs as a daemon and exposes Linux OS primitives as MCP tools over HTTP/SSE on a configurable port (default 8888).

Each tool is implemented as a stateless handler function with the signature:

```cpp
mcp::json handler(const mcp::json& params, const std::string& session_id);
```

Tools read directly from Linux kernel interfaces where possible:

- `sysinfo`: reads `/proc/stat` (CPU, sampled over 100ms), `sys/sysinfo.h` sysinfo(2) syscall (memory, load averages, uptime), `statvfs(2)` (disk)
- `process_list`: reads `/proc/<PID>/stat` directly, parsing the kernel-defined stat format
- `network_info`: reads `/sys/class/net/*/operstate|address|mtu` and `/proc/net/dev` for traffic counters
- `journal_query`: invokes `journalctl --no-pager` (falls back to `/var/log/syslog`)
- `exec`: invokes the system `timeout(1)` utility to enforce process-level timeouts
- `read_file`, `write_file`, `list_dir`: wrap `std::filesystem` (C++17)

The choice to read kernel interfaces directly rather than invoking tools like `top` or `free` is intentional: it eliminates parsing ambiguity, reduces latency, and produces structured data in a format that LLMs parse reliably (JSON).

New tools are added by creating one `.cpp` file following the established pattern, adding it to CMakeLists.txt, declaring it in `tools.h`, and registering it in `main.cpp`. No changes to existing code are required.

### 4.3 Agent Definition Format

Agents are defined as YAML files:

```yaml
name: sysmonitor
description: System health monitor
model: default           # resolved to LLM URL at runtime
tools:                   # capability allowlist
  - sysinfo
  - process_list
  - journal_query
  - exec
priority: normal         # low | normal | high | realtime
context_limit: 4096      # tokens available per run
max_steps: 8             # maximum tool-call cycles before forced termination
system_prompt: |
  You are a system monitoring agent for NeuralOS...
```

The `tools` field implements P3: the agent can only call listed tools, enforced by the AgentInstance before making MCP calls. The `max_steps` field implements a resource quota: agents cannot run inference loops indefinitely. The `context_limit` field bounds token usage per request.

Agent YAML files are designed to be human-readable, version-controllable, and machine-generatable (by the builder agent).

### 4.4 AgentInstance: The Process Analog

`AgentInstance` is the C++ class that runs a single agent. It holds:

- The parsed `AgentConfig`
- An `LLMClient` connected to the configured LLM backend
- A persistent `mcp::sse_client` connection to nos-server
- The current conversation history (rebuilt fresh per request)

On construction, `AgentInstance` connects to nos-server via MCP SSE, fetches the tool list, and filters it to the agent's allowlist. This connection is persistent across requests: the MCP session is established once and reused.

The inference loop (`run_loop`) operates as follows:

```
1. Build initial history: [system_prompt, user_message]
2. For step in range(max_steps):
   a. Call LLM: llm_.complete(history, tools_schema)
   b. If no tool calls in response: return response.content
   c. For each tool call:
      - Execute via mcp_->call_tool(name, args)
      - Append assistant turn (with tool_calls) to history
      - Append tool result turn to history
3. Return max_steps-exceeded message
```

This loop is the agent equivalent of a process's instruction execution cycle. The MCP tool call is the system call.

The `LLMClient` uses the vendored `httplib` for HTTP requests to the LLM backend. HTTPS is supported via OpenSSL (defined uniformly via CMake's `target_compile_definitions` to avoid ODR violations that would otherwise cause heap corruption at runtime — a non-obvious C++ pitfall we encountered and documented).

### 4.5 nos-supervisor: The Scheduler and Supervisor

`nos-supervisor` manages the agent registry and routes requests. It is implemented as a C++ HTTP server using httplib.

**Agent registry**: A `std::map<std::string, std::shared_ptr<AgentInfo>>` protected by a mutex. Each `AgentInfo` holds the config, current status, run statistics, and a `std::unique_ptr<AgentInstance>` (null until first use — lazy initialization).

**Lifecycle management**: Agent YAMLs are loaded from a directory at startup and on `POST /agents/reload`. Agents are instantiated lazily on first request, not at load time. This means the supervisor can load many agent definitions without establishing MCP or LLM connections until they are actually needed.

**Concurrency control**: Each `AgentInfo` has a `std::mutex` (`run_mutex`). A run request acquires this mutex exclusively, preventing concurrent execution of the same agent. Multiple requests to the same agent queue naturally through the mutex. Different agents run concurrently (one thread per active request, using httplib's built-in thread pool).

**Error recovery**: If an agent throws during execution, the supervisor catches the exception, marks the agent as `error`, resets the `AgentInstance` (which releases the MCP session), and returns the error to the caller. The next request to that agent will re-instantiate it, establishing a fresh MCP session. This is analogous to a process supervisor restarting a crashed daemon.

**Priority**: The `AgentConfig::priority` field (low / normal / high / realtime) is stored in the registry. In the current implementation it is informational; the roadmap includes a priority queue at the request dispatch level, analogous to Linux's `nice` and scheduling classes.

### 4.6 nos CLI and nos-builder

`nos` is a C++ binary that communicates with nos-supervisor via HTTP. It provides a human-friendly interface for the five most common operations: listing agents, running an agent, running the default agent, reloading, and checking system status.

`nos-builder` is a thin wrapper that runs the builder agent — a NeuralOS agent whose system prompt instructs it to create agent YAML files from natural language descriptions, using the `write_file` tool to persist them. After execution, `nos-builder` compares the `agents/` directory before and after and prints any newly created files for review.

---

## 5. Implementation

### 5.1 Technology Choices

**Language: C++17.** The choice of C++ over Python is motivated by: (a) reduced interpreter overhead, particularly relevant for the supervisor's dispatch hot path; (b) native access to Linux kernel APIs (`sysinfo(2)`, `statvfs(2)`, direct `/proc` reads) without FFI; (c) compatibility with llama.cpp and the cpp-mcp library; (d) the eventual goal of kernel-adjacent integration where Python is not appropriate.

**MCP library: cpp-mcp (vendored).** The cpp-mcp library (MIT, github.com/hkr04/cpp-mcp) is vendored into `third-party/cpp-mcp/` rather than used as a submodule. This choice was made after encountering a push permission error with the submodule approach and an API mismatch between the local copy and the upstream version. Vendoring gives full control over the version used, eliminates runtime dependency resolution, and allows direct modification if needed.

**LLM backend: OpenAI-compatible HTTP API.** The `LLMClient` class makes standard HTTP POST requests to `/v1/chat/completions`. This is the de facto standard interface supported by llama-server (llama.cpp), Groq, the Anthropic API, OpenAI, and dozens of other providers. Backend selection is entirely configuration-time: changing `NOS_LLM_URL` switches the inference provider without recompilation.

**Build system: CMake.** A notable implementation detail: `CPPHTTPLIB_OPENSSL_SUPPORT` must be defined as a `PUBLIC` compile definition on the `mcp` CMake target so that it propagates to all translation units that include `httplib.h`. Defining it only in one TU while other TUs include the same header without it constitutes an ODR violation that manifests as `malloc(): invalid size (unsorted)` at runtime — a heap corruption caused by inconsistent `httplib::Client` class layouts across translation units.

### 5.2 Hardware Requirements

NeuralOS is designed to run on commodity CPU-only hardware. The reference development machine is a ThinkPad T14 Gen 1 with a Ryzen 5 PRO 4650U and 16GB RAM. The entire stack — nos-server, nos-supervisor, and llama-server with a 4-bit quantized 4B parameter model — runs concurrently on this hardware with acceptable inference latency (2-8 seconds per agent turn).

The LLM backend is intentionally separate from the NeuralOS stack. This enables:
- Local inference with llama.cpp on CPU
- Local inference with GPU acceleration when available
- Remote inference via Groq (free tier: ~300 tokens/second on Llama 3.1 70B)
- Remote inference via any cloud API

No GPU is required to run NeuralOS. This is a deliberate accessibility choice: the architecture should work on the hardware most people have, not only on expensive GPU workstations.

---

## 6. The Agent Lifecycle in Practice

A complete agent interaction under NeuralOS follows this sequence:

1. User runs `nos ask "What processes are using the most RAM?"`
2. `nos` sends `POST /agents/sysmonitor/run` with `{"message": "..."}` to nos-supervisor
3. nos-supervisor looks up `sysmonitor` in the registry, acquires its `run_mutex`
4. nos-supervisor calls `ensure_instance(sysmonitor)` — establishes MCP session with nos-server if not already connected
5. nos-supervisor calls `agent.run(message)`, which:
   a. Sends system prompt + user message to LLM
   b. LLM responds: call `process_list({"limit": 10})`
   c. AgentInstance calls `mcp_->call_tool("process_list", {"limit": 10})`
   d. nos-server reads `/proc/<PID>/stat` for each process, returns sorted JSON
   e. AgentInstance appends tool result to history, calls LLM again
   f. LLM produces final text: "The top memory consumers are: firefox (512MB, PID 1234)..."
6. nos-supervisor releases `run_mutex`, returns response via HTTP
7. `nos` prints response to stdout

Total time on the reference hardware with llama-server (gemma-3-4b): approximately 3-5 seconds.

---

## 7. Extensibility

### 7.1 Adding Tools

A new nos-server tool requires:
1. One `.cpp` file implementing a handler function and a `register_*()` function
2. One entry in `src/nos-server/CMakeLists.txt`
3. One declaration in `tools/tools.h`
4. One call to `register_*()` in `main.cpp`

No existing code changes are needed. The tool becomes immediately available to all agents that list it in their `tools` allowlist.

### 7.2 Adding MCP Servers

Any MCP-compatible server can be used as a tool source alongside nos-server. The AgentInstance can be configured with multiple MCP server connections. This enables transparent use of the 17,000+ community-built MCP servers without modification to NeuralOS itself.

### 7.3 Adding Channels

nos-supervisor exposes a standard HTTP REST API. Any system that can make HTTP POST requests can dispatch agent runs. Telegram bots, Slack apps, email gateways, voice interfaces, web frontends — all are implemented as thin clients to the supervisor API, not as modifications to the agent infrastructure.

### 7.4 Adding Agents

The builder agent turns natural language descriptions into working agent YAML files. This is the primary creation mechanism for non-programmers. Advanced users can write YAML directly or version-control agent definitions and deploy them to the supervisor via reload.

---

## 8. Roadmap

### 8.1 Immediate (Current Development)

- Persistent agent memory across requests (embedded vector store, agent-local KV)
- Authentication and access control on the supervisor HTTP API
- Parallel agent execution (multiple instances of the same agent)
- Metrics endpoint for monitoring (Prometheus-compatible)

### 8.2 Near-Term

- **sched_ext integration**: A BPF scheduler program that identifies NeuralOS agent processes and applies agent-aware scheduling policies — prioritizing inference-phase processes, deprioritizing waiting-phase processes, and enforcing priority classes at the kernel level. This is the "LLM at the kernel level" thesis made concrete.
- **Distributed supervisor**: Multiple nos-supervisor instances sharing an agent registry, enabling agents to run across a network of machines with transparent routing.
- **Multi-agent workflows**: Chained agent execution, where one agent can dispatch requests to another via the supervisor API.
- **Agent versioning**: YAML definitions stored in git, with the supervisor supporting atomic rollout of new agent definitions.

### 8.3 Research Directions

- **Context-aware scheduling**: Scheduling decisions informed by the agent's current phase (prompt evaluation, token generation, tool execution, waiting) — the Agent.xpu approach applied to nos-supervisor.
- **Federated agent registry**: Multiple NeuralOS nodes sharing a common agent namespace, with routing based on capability, load, and locality.
- **Kernel memory pressure integration**: When the system is under memory pressure, the supervisor can proactively reduce context limits or shed low-priority agent requests before the OOM killer acts.

---

## 9. Discussion

### 9.1 Why Not Python?

The question of implementation language is worth addressing directly. The existing agent framework ecosystem is predominantly Python, and Python has genuine advantages for this use case: rapid iteration, a rich library ecosystem, and the availability of AIOS and other reference implementations.

NeuralOS chooses C++ for reasons that become more important as the system matures. The eventual integration with sched_ext and eBPF requires C/C++ or BPF bytecode — Python is not an option at the kernel interface. The goal of running on resource-constrained hardware (old ThinkPads, embedded systems) is better served by a compiled language. And the architectural aspiration — to become a component of the Linux platform rather than an application running on top of it — is more credible in the language the platform is written in.

This is not an argument that Python is wrong for agents in general; it is an argument that the *OS layer* of an agent system should be implemented at the OS level.

### 9.2 The "Just Use a Framework" Objection

The most common objection to NeuralOS's approach is that existing frameworks can be extended to provide the missing properties. LangChain can be given better resource management. CrewAI can be given a restart policy. AutoGPT can be given a priority queue.

This objection mistakes the symptom for the cause. The missing properties are not absent because framework developers forgot to add them — they are absent because frameworks are the wrong architectural layer for these concerns. A process scheduler cannot be usefully implemented as a library function because scheduling is a kernel-level concern. Similarly, agent lifecycle management cannot be usefully implemented inside an agent framework because the framework itself is the scope of the problem. The supervisor needs to be *outside* the agents it manages, just as init is outside the processes it manages.

The result of adding lifecycle management to a framework is a heavier framework, not an OS. NeuralOS is not a heavier framework.

### 9.3 Relationship to "AI PC" Products

The commercial AI PC products (Microsoft Copilot+, Apple Intelligence) are not wrong; they are solving a different problem. They are making existing applications more intelligent, which is valuable. NeuralOS is doing something orthogonal: providing an infrastructure layer for agents that operate *independently* of any specific application, with access to system resources rather than application-specific context.

The analogy: Apple Intelligence and Copilot+ are intelligent features of individual applications. NeuralOS is the process table and scheduler that makes independent agent processes possible.

---

## 10. Conclusion

The limitations of current agent frameworks are not implementation deficiencies; they are architectural consequences of building at the wrong layer. Agents need the properties that processes have always had — named existence, supervised lifecycle, resource quotas, capability-controlled system access, and standard interfaces — and these properties belong in the operating system, not in application libraries.

NeuralOS demonstrates that implementing these properties on Linux is feasible on commodity hardware, using existing open standards (MCP), established protocols (OpenAI chat completions API), and a minimal C++ codebase. The full stack — MCP tool server, agent runtime, supervisor, CLI, and builder agent — compiles and runs on a CPU-only ThinkPad from 2020.

The architecture is open for extension in every dimension: new tools extend nos-server, new agents extend the YAML registry, new channels extend the supervisor's HTTP API, and new LLM backends are a URL change. The protocol (MCP) is a Linux Foundation standard with 97 million monthly downloads, ensuring that the NeuralOS tool interface is compatible with the broader ecosystem rather than a walled garden.

The path toward kernel integration via sched_ext is clear and supported by peer-reviewed research. The path toward distributed operation is a natural extension of the supervisor's HTTP API. The path toward persistent agent memory is well-trodden by existing vector store and KV store technology.

What NeuralOS proposes is not a revolution in AI — the models, the inference engines, and the protocols already exist. It proposes a revolution in how AI runs: not as a feature bolted onto an existing OS, but as a first-class concern of the system layer, managed with the same rigor that operating systems have always brought to the resources they manage.

---

## References

[1] Mei, K., Zhu, X., Xu, W., Jin, M., Hua, W., Li, Z., Xu, S., Ye, R., Ge, Y., Zhang, Y. (2025). "AIOS: LLM Agent Operating System." *Conference on Language Modeling (COLM 2025)*. arXiv:2403.16971.

[2] Zheng, Y., et al. (2025). "Towards Agentic OS: An LLM Agent Framework for Linux Schedulers." arXiv:2509.01245.

[3] Chen, S., et al. (2025). "Agent.xpu: Efficient Scheduling of Agentic LLM Workloads on Heterogeneous SoC." arXiv:2506.24045.

[4] Anthropic. (2024). "Introducing the Model Context Protocol." https://www.anthropic.com/news/model-context-protocol.

[5] LangChain. (2024). "State of AI Agents Report 2024." https://www.langchain.com/stateofaiagents.

[6] Karpathy, A. (2023). LLM OS concept. X (formerly Twitter). https://x.com/karpathy/status/1707437820045062561.

[7] Linux Kernel Documentation. (2024). "sched-ext: Scheduler Extensions." https://docs.kernel.org/scheduler/sched-ext.html.

[8] Anthropic. (2025). "Donating the Model Context Protocol to the Agentic AI Foundation." https://www.anthropic.com/news/donating-the-model-context-protocol.

[9] Pento. (2025). "A Year of MCP: From Internal Experiment to Industry Standard." https://www.pento.ai/blog/a-year-of-mcp-2025-review.

[10] LeCun, Y. (2025). Predictions on LLM limitations. TechCrunch, January 2025.
