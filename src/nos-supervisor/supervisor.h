// =============================================================================
// src/nos-supervisor/supervisor.h — NeuralOS Agent Supervisor
// =============================================================================
//
// nos-supervisor is the central process manager for NeuralOS agents. It plays
// the role that systemd plays for services, but for LLM agents.
//
// Responsibilities:
//   - Load agent definitions (.yaml) from a directory at startup and on demand
//   - Maintain a registry of known agents and their current status
//   - Accept run requests via HTTP API and dispatch them to agents
//   - Enforce per-agent concurrency (one active run at a time per agent)
//   - Priority queue: realtime > high > normal > low
//   - Expose a REST API consumed by nos-cli and external tools
//
// HTTP API (all JSON):
//   GET  /agents                  → list all agents with status
//   POST /agents/{name}/run       → { "message": "..." } → { "response": "..." }
//   GET  /agents/{name}           → single agent info
//   POST /agents/reload           → rescan agents directory
//   GET  /health                  → { "status": "ok", "agents": N }
//
// Agent status lifecycle:
//   idle → running → idle   (normal)
//   idle → running → error  (LLM/MCP failure)
//
// Design notes:
//   - Each agent has a dedicated mutex: concurrent requests queue until the
//     agent is free. This is intentionally simple for v1; v2 can add pooling.
//   - Priority queue uses std::priority_queue ordered by AgentPriority.
//   - The HTTP server (httplib) runs on its own thread pool.
//   - AgentInstances are created lazily on first use and cached.
// =============================================================================

#pragma once
#include "../agent/agent_config.h"
#include "../agent/agent.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <functional>

// ── Agent status ──────────────────────────────────────────────────────────────

enum class AgentStatus {
    idle,       // loaded and ready
    running,    // currently processing a request
    error,      // last run failed
    unloaded    // config known but instance not yet created
};

struct AgentInfo {
    AgentConfig  config;
    AgentStatus  status      = AgentStatus::unloaded;
    std::string  last_error;
    int          run_count   = 0;
    int          error_count = 0;

    std::unique_ptr<AgentInstance> instance;  // null if not yet instantiated
    std::mutex                     run_mutex; // prevents concurrent runs

    // Non-copyable because of mutex and unique_ptr
    AgentInfo() = default;
    AgentInfo(const AgentInfo&) = delete;
    AgentInfo& operator=(const AgentInfo&) = delete;
};

// ── Supervisor ────────────────────────────────────────────────────────────────

class Supervisor {
public:
    Supervisor(const std::string& agents_dir,
               const std::string& nos_server_url,
               const std::string& default_llm_url,
               const std::string& default_api_key);

    // Load / reload all .yaml files from agents_dir_
    // Returns number of agents loaded.
    int load_agents();

    // Run an agent by name with a user message.
    // Blocks until the agent finishes (uses agent's own thread).
    // Returns the agent's text response.
    // Throws std::runtime_error if agent not found or run fails.
    std::string run_agent(const std::string& name, const std::string& message,
                          bool verbose = false);

    // Return a snapshot of all agent statuses (thread-safe)
    struct AgentSnapshot {
        std::string  name;
        std::string  description;
        std::string  model;
        std::string  status;     // "idle" | "running" | "error" | "unloaded"
        std::string  last_error;
        int          run_count;
        int          error_count;
        int          priority;
    };
    std::vector<AgentSnapshot> list_agents() const;

    // Return status for a single agent (empty string status = not found)
    AgentSnapshot agent_info(const std::string& name) const;

private:
    std::string agents_dir_;
    std::string nos_server_url_;
    std::string default_llm_url_;
    std::string default_api_key_;

    // Registry: agent name → AgentInfo
    mutable std::mutex registry_mutex_;
    std::map<std::string, std::shared_ptr<AgentInfo>> registry_;

    // Ensure an AgentInstance exists for the given info (lazy init)
    // Must be called with info.run_mutex held.
    void ensure_instance(AgentInfo& info);

    static std::string status_str(AgentStatus s);
};
