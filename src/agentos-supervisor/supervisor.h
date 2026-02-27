// =============================================================================
// src/agentos-supervisor/supervisor.h — AgentOS Agent Supervisor
// =============================================================================
//
// agentos-supervisor is the central process manager for AgentOS agents. It plays
// the role that systemd plays for services, but for LLM agents.
//
// Responsibilities:
//   - Load agent definitions (.yaml) from a directory at startup and on demand
//   - Maintain a registry of known agents and their current status
//   - Accept run requests via HTTP API and dispatch them to agents
//   - Support parallel execution: multiple agents (and the same agent) can run
//     concurrently — each request gets its own AgentInstance
//   - Load/save conversation history via AgentMemory for cross-run memory
//   - Expose a REST API consumed by agentos-cli and external tools
//
// HTTP API (all JSON):
//   GET  /agents                     → list all agents with status
//   POST /agents/{name}/run          → { "message": "..." } → { "response": "..." }
//   GET  /agents/{name}              → single agent info
//   POST /agents/reload              → rescan agents directory
//   POST /agents/{name}/memory/clear → clear conversation history for agent
//   GET  /health                     → { "status": "ok", "agents": N }
//
// Parallel execution design (v1.1):
//   Each run() call creates a fresh AgentInstance (no caching). Conversation
//   history is loaded from AgentMemory (file-locked JSON) before the run and
//   saved after. The registry_mutex_ is held only briefly at run start/end
//   (never during LLM inference), so multiple concurrent requests proceed
//   without blocking each other.
//
// Agent status lifecycle:
//   unloaded → running → idle   (first run)
//   idle     → running → idle   (subsequent runs)
//   running  → error            (LLM/MCP failure)
// =============================================================================

#pragma once
#include "../agent/agent_config.h"
#include "../agent/agent.h"
#include "../agent/agent_memory.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <atomic>

// ── Agent status ──────────────────────────────────────────────────────────────

enum class AgentStatus {
    idle,       // loaded and ready (no active runs)
    running,    // at least one run active
    error,      // last completed run failed
    unloaded    // config known but never successfully run
};

struct AgentInfo {
    AgentConfig  config;
    AgentStatus  status      = AgentStatus::unloaded;
    std::string  last_error;
    int          run_count   = 0;
    int          error_count = 0;
    int          active_runs = 0;   // incremented per concurrent request

    // Copyable (no mutex or unique_ptr — those were removed for parallel design)
    AgentInfo() = default;
};

// ── Supervisor ────────────────────────────────────────────────────────────────

class Supervisor {
public:
    Supervisor(const std::string& agents_dir,
               const std::string& agentos_server_url,
               const std::string& default_llm_url,
               const std::string& default_api_key);

    // Load / reload all .yaml files from agents_dir_.
    // Returns number of agents loaded.
    int load_agents();

    // Run an agent by name with a user message.
    // Creates a fresh AgentInstance per call (parallel-safe).
    // Loads memory before run, saves after. Throws on failure.
    std::string run_agent(const std::string& name, const std::string& message,
                          bool verbose = false);

    // Clear conversation memory for an agent.
    void clear_memory(const std::string& name);

    // Return a snapshot of all agent statuses (thread-safe).
    struct AgentSnapshot {
        std::string  name;
        std::string  description;
        std::string  model;
        std::string  status;     // "idle" | "running" | "error" | "unloaded"
        std::string  last_error;
        int          run_count;
        int          error_count;
        int          active_runs;
        int          priority;
    };
    std::vector<AgentSnapshot> list_agents() const;

    // Return status for a single agent (empty name in snapshot = not found).
    AgentSnapshot agent_info(const std::string& name) const;

private:
    std::string agents_dir_;
    std::string agentos_server_url_;
    std::string default_llm_url_;
    std::string default_api_key_;

    // Registry: agent name → AgentInfo (all access under registry_mutex_)
    mutable std::mutex registry_mutex_;
    std::map<std::string, std::shared_ptr<AgentInfo>> registry_;

    static std::string status_str(AgentStatus s);
};
