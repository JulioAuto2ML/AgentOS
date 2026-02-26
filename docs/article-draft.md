# What If Your Operating System Could Think?

### The case for treating AI agents as first-class OS citizens — and why nobody has done it right yet

---

There's a moment in computing history that everyone points to as the turning point. It wasn't the invention of the transistor, or the internet, or even the graphical interface. It was the moment when the operating system stopped being a program you ran and became the invisible foundation that made everything else possible — the layer you didn't notice because it was doing its job perfectly.

We may be living through that same moment again, and most people haven't noticed.

---

## The Idea That Started This

About a year ago, I started thinking about a question that sounded almost ridiculous: *what would Linux look like if it had been designed knowing that LLMs existed?*

Not "Linux with an AI assistant bolted on" — every major OS vendor is doing that. Something more fundamental. What if the operating system treated an AI agent the same way it treats a process? Named, isolated, scheduled, supervised, with resource quotas, lifecycle management, and a well-defined interface for interacting with the rest of the system?

The question has a name now. Andrej Karpathy articulated it publicly in late 2023: the LLM is the kernel of a new operating system. "With many components dropping recently, a more complete picture is emerging of LLMs not as a chatbot, but the kernel process of a new Operating System." He sketched out the hardware spec — a 256-core processor running at 20Hz (token generation speed), 128K tokens of RAM, embeddings as the filesystem — and the idea stuck with me.

But Karpathy was describing an analogy. What I wanted to build was the real thing.

---

## Why Current Solutions Miss the Point

Before explaining what NeuralOS is, it helps to understand what's wrong with how we run AI agents today.

The dominant approach is to use a framework — LangChain, CrewAI, AutoGPT, LlamaIndex, take your pick. These are application-level libraries. You write Python code that creates agents, chains together prompts, and calls tools. It works, and for many use cases it works well. But it has a fundamental architectural flaw: agents live in your application, not in your system.

Consider what this means in practice. When you have two agents running simultaneously — say, one monitoring your logs and another answering a user question — they are invisible to each other and to the OS. The kernel schedules their Python threads with no knowledge of what they're actually doing. There's no concept of "this agent is doing expensive inference right now, so deprioritize the other one." No concept of "this agent has exceeded its resource quota." No mechanism for an agent to yield control and resume later without losing its context. No standard way for agents to call system primitives safely. No supervisor that can restart a crashed agent, or refuse to start a low-priority one when the system is under load.

The [State of AI Agents report](https://www.langchain.com/stateofaiagents) from LangChain, one of the most widely-used frameworks, is remarkably honest about these limitations: the number-one complaint from agent builders is "performance quality," and the hardest problem is "making sure the LLM has appropriate context at each step." The frameworks are hitting a ceiling — and that ceiling is the OS.

The situation with commercial "AI PCs" is even more telling. Microsoft Copilot+ PCs ship with NPUs sitting largely unused. Apple Intelligence represents genuine integration within the Apple ecosystem, but it's still an overlay on top of a conventional OS. A survey from Canalys found that 50% of US online adults don't understand why they want an AI PC. They're right to be confused: current "AI computers" are just regular computers with AI features added, not systems designed from the ground up to run AI workloads as their primary purpose.

---

## What Academic Research Has Found

Researchers have been circling this problem too. The most significant work is AIOS — the LLM Agent Operating System — published at COLM 2025 by Mei et al. from Rutgers University. AIOS proposes an OS kernel for agents that provides scheduling, context management (saving and restoring LLM generation progress, like context-switching for processes), memory management with short- and long-term storage per agent, and access control. Their results are compelling: up to 2.1x faster execution compared to traditional agent frameworks, with better resource utilization across concurrent workloads.

More recent work goes further. A September 2025 paper, "Towards Agentic OS," introduced SchedCP — a framework that uses LLM agents to optimize Linux kernel schedulers themselves, communicating with the kernel via MCP (Model Context Protocol) and writing eBPF scheduler policies autonomously. The results: a 1.79x performance improvement in workload execution and a 13x cost reduction versus naive agentic approaches.

And then there's Agent.xpu, published in June 2025, which addresses the specific challenge of running reactive (user-initiated, low-latency) and proactive (background, high-throughput) AI workloads concurrently on commodity heterogeneous hardware — CPU, integrated GPU, and NPU — without them stepping on each other. This is exactly the scheduling problem that needs to be solved for AI to feel native on a computer, not bolted on.

These are peer-reviewed results confirming what the architecture demands: we need OS-level primitives for AI, not just application-level frameworks.

---

## The Standard That Made This Possible

One development in late 2024 changed the technical landscape significantly: Anthropic released the Model Context Protocol (MCP), an open standard for how AI systems communicate with tools and data sources via JSON-RPC 2.0 over HTTP/SSE. It was designed as an internal experiment and released publicly in November 2024. By April 2025 it had been downloaded 8 million times. By December 2025 it had 97 million monthly SDK downloads, 10,000+ server implementations, and was supported natively by Claude, ChatGPT, Gemini, Cursor, and VS Code. OpenAI, Google DeepMind, and Block co-founded the Agentic AI Foundation under the Linux Foundation to make MCP a genuinely neutral open standard.

MCP solves exactly the right problem at exactly the right level. It's not a programming model or a framework — it's a protocol. An MCP server exposes capabilities (tools, resources, prompts) over a standard interface. Any client that speaks MCP — any LLM agent, any tool, any service — can use those capabilities without knowing anything about the implementation. This is the Unix pipe philosophy applied to AI: do one thing, speak a standard protocol, compose freely.

NeuralOS uses MCP as the boundary between agents and the operating system. The `nos-server` component exposes Linux system primitives as MCP tools — `sysinfo`, `process_list`, `exec`, `read_file`, `journal_query`, `network_info`, and more. Any MCP-compatible agent can call these tools. Any MCP-compatible tool server — including the thousands of community-built ones — can be plugged in.

---

## NeuralOS: What It Actually Is

NeuralOS is a C++ daemon stack that brings OS-level thinking to LLM agents on Linux. It is not a new Linux distribution. It is not a modified kernel. It is a set of components that run alongside your existing system and give agents the properties that processes have always had.

The design maps directly to OS concepts:

| OS concept | NeuralOS equivalent |
|---|---|
| Process | `AgentInstance` — a running LLM with a tool loop |
| Scheduler | `nos-supervisor` — priority queue, resource quotas |
| System call | MCP tool call — exec, read_file, sysinfo, etc. |
| /proc filesystem | Agent registry — name, status, run count |
| init / systemd | `nos-supervisor` — start, stop, restart, reload |
| Shell | `nos` CLI — human interface to the supervisor |

The stack has five components:

**nos-server** is an MCP tool server that exposes OS primitives. It runs as a daemon and speaks the MCP protocol over HTTP/SSE. Agents connect to it and call tools. The tool list is extensible: adding a new system capability is a matter of writing one C++ file following a simple pattern.

**AgentInstance** is the runtime for a single agent. It holds the LLM client, the conversation history, the tool loop, and the configuration. On each turn it calls the LLM backend (any OpenAI-compatible API — local llama-server, Groq, Anthropic, anything), checks if the response contains tool calls, executes them via nos-server using MCP, appends results to the context, and repeats until the model produces a final answer.

**nos-supervisor** is the agent manager — the systemd for LLM agents. It loads agent definitions from YAML files, maintains a registry of agents with their status (idle, running, error), accepts run requests via an HTTP REST API, and enforces per-agent concurrency. A priority queue ensures that high-priority agents don't wait behind low-priority background tasks. When an agent crashes, the supervisor logs the error, resets its instance, and makes it available for the next request. Agents are reloaded from disk without restarting the supervisor.

**nos-builder** is the agent creation tool. It runs the builder agent, which takes a natural language description and writes a complete YAML definition to `agents/`. "Create an agent that monitors disk usage and alerts when above 80%" produces a working `disk-monitor.yaml` with the right tool allowlist, a focused system prompt, and appropriate step limits. This is the user-facing creation interface that makes agents accessible without programming.

**nos** is the CLI. `nos agents` shows a table of loaded agents with status, run counts, and error counts. `nos run sysmonitor "What is eating my RAM?"` dispatches a request to the supervisor and prints the response. `nos ask "How is the system doing?"` routes to the default agent. `nos status` checks whether nos-server and nos-supervisor are reachable. One binary, five commands, zero configuration required.

Agent definitions are YAML files:

```yaml
name: sysmonitor
description: System health monitor
model: default
tools: [sysinfo, process_list, journal_query, exec]
priority: normal
max_steps: 8
system_prompt: |
  You are a system monitoring agent for NeuralOS...
```

The `tools` field is an allowlist. The agent can only call the tools listed, even if nos-server exposes more. This is capability-based security: agents get exactly the access they need, nothing more.

---

## The Possibilities

This architecture opens directions that current frameworks can't reach.

**Connecting new channels.** Because nos-supervisor exposes a standard HTTP REST API, adding a new input channel — Telegram, Slack, email, a web interface, a voice interface — is a matter of writing a client that calls `/agents/{name}/run`. The agent logic is completely separate from the channel. A Telegram bot that routes messages to the right NeuralOS agent is fewer than 50 lines of Python. The same agents that answer CLI queries also serve the chat interface.

**Using existing MCP servers.** The MCP ecosystem has thousands of servers: databases, cloud providers, version control systems, communication tools, document stores. Any of these can be exposed as tools to NeuralOS agents. A `git` MCP server gives agents the ability to commit code. A `postgres` MCP server gives agents database access. A `slack` MCP server lets agents read and write to Slack channels. The protocol is the integration layer — no custom code needed on the agent side.

**Multi-agent workflows.** The supervisor can be extended to support chained agents, where the output of one agent becomes the input to another. A "triage" agent decides which specialist agent should handle a request. A "planner" agent breaks a task into subtasks and dispatches them. The infrastructure for this is already in place — it's a routing problem, and the supervisor is already a router.

**Kernel integration.** The research on sched_ext (Linux 6.12's extensible scheduler) points toward a future where NeuralOS doesn't just sit alongside the kernel but participates in it. An agent workload aware scheduler — one that knows whether a process is running inference, tool execution, or waiting for results — can make smarter scheduling decisions than any general-purpose scheduler. This is the "LLM at the kernel level" that started this project: not literally inside the kernel, but participating in kernel decisions in a way that general-purpose schedulers cannot.

**The building block model.** The architecture is composable. A new tool is one C++ file. A new agent is one YAML file. A new channel is a few dozen lines in any language that speaks HTTP. A new LLM backend is a URL and an API key. The components are small, well-defined, and replaceable. This is different from framework-level agent systems, where changing the model or the tool interface often requires rewriting significant portions of the agent logic.

---

## What Makes This Different

There are four qualities that distinguish NeuralOS from the existing landscape, and all four come from choosing the OS as the conceptual model rather than the application.

**Agents are system entities, not library objects.** A NeuralOS agent exists as a named entry in a registry, survives individual requests, accumulates state (run count, error history), can be queried, started, stopped, and reloaded without touching application code. This is the difference between a systemd service and a function call.

**The LLM backend is completely decoupled.** The agent definition says `model: default`. The supervisor resolves "default" to whatever LLM URL is configured at deployment time — a local 4-bit quantized Gemma running on a laptop, or a 70B model on Groq, or Claude via the Anthropic API. The agents don't know and don't care. This means the same agent YAML works on a $200 Raspberry Pi (with a small model) and on a cloud GPU (with a large model). The intelligence scales with the hardware.

**Resource management is built in.** Context budgets, step limits, concurrency control, priority queues — these are configuration, not code. The supervisor enforces them. An agent that exceeds its step limit gets a graceful termination message, not an infinite loop.

**The protocol is an open standard.** MCP is not a NeuralOS invention. It's a Linux Foundation standard with 97 million monthly downloads and support from every major AI provider. This means NeuralOS is not a walled garden. Every MCP server ever built is immediately usable. Every MCP client can talk to nos-server. The ecosystem is already enormous and growing.

---

## The Path Forward

This is early work. The current implementation has one agent running at a time per supervisor instance (concurrency within the queue, not parallel execution), a simple HTTP API without authentication, no persistent agent memory across restarts, and no kernel-level integration yet. These are the next milestones.

But the foundation is correct. The components are small and composable. The protocol is standard and open. The agent definition format is human-readable YAML. The whole stack runs on hardware that's four years old and has no GPU.

The question Karpathy posed in 2023 — what does an operating system look like when the LLM is the kernel? — has a partial answer now. It looks like a named registry and a priority queue and a standard protocol and a YAML file. It looks like `nos ask "How is the system doing?"` and getting a real answer in two seconds from an agent that checked the CPU, the memory, the top processes, and the recent error logs before responding.

It looks, in other words, like computing usually does when you strip away the hype: a set of simple, composable pieces doing one thing each, connected by a protocol that everyone agrees on.

The operating system didn't change the world by being clever. It changed the world by being the right foundation at the right time.

---

*NeuralOS is open source. The code, agent definitions, documentation, and build instructions are at [github.com/JulioAuto2ML/NeuralOS](https://github.com/JulioAuto2ML/NeuralOS).*

---

### References

1. Karpathy, A. (2023). "LLM OS" concept posts. X (formerly Twitter).
2. Mei, K., et al. (2025). "AIOS: LLM Agent Operating System." COLM 2025. arXiv:2403.16971.
3. Zheng, Y., et al. (2025). "Towards Agentic OS: An LLM Agent Framework for Linux Schedulers." arXiv:2509.01245.
4. Chen, S., et al. (2025). "Agent.xpu: Efficient Scheduling of Agentic LLM Workloads on Heterogeneous SoC." arXiv:2506.24045.
5. Anthropic. (2024). "Introducing the Model Context Protocol." anthropic.com/news/model-context-protocol.
6. LangChain. (2024). "State of AI Agents Report." langchain.com/stateofaiagents.
7. Linux Kernel Documentation. (2024). "sched-ext: Scheduler Extensions." docs.kernel.org/scheduler/sched-ext.html.
8. Anthropic. (2025). "Donating MCP to the Agentic AI Foundation." anthropic.com/news/donating-the-model-context-protocol.
