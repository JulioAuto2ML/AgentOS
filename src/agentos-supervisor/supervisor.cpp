// =============================================================================
// src/agentos-supervisor/supervisor.cpp
// =============================================================================

#include "supervisor.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstdlib>

namespace fs = std::filesystem;

// ── Constructor ───────────────────────────────────────────────────────────────

Supervisor::Supervisor(const std::string& agents_dir,
                       const std::string& agentos_server_url,
                       const std::string& default_llm_url,
                       const std::string& default_api_key)
    : agents_dir_(agents_dir)
    , agentos_server_url_(agentos_server_url)
    , default_llm_url_(default_llm_url)
    , default_api_key_(default_api_key)
{}

// ── Agent loading ─────────────────────────────────────────────────────────────

int Supervisor::load_agents() {
    if (!fs::exists(agents_dir_) || !fs::is_directory(agents_dir_))
        throw std::runtime_error("Agents directory not found: " + agents_dir_);

    std::lock_guard<std::mutex> lock(registry_mutex_);
    int loaded = 0;

    for (const auto& entry : fs::directory_iterator(agents_dir_)) {
        if (entry.path().extension() != ".yaml" &&
            entry.path().extension() != ".yml")
            continue;

        try {
            AgentConfig cfg = AgentConfig::from_file(entry.path().string());

            if (registry_.count(cfg.name)) {
                // Update config; preserve run stats
                registry_[cfg.name]->config = cfg;
                std::cerr << "[supervisor] Reloaded agent: " << cfg.name << "\n";
            } else {
                auto info    = std::make_shared<AgentInfo>();
                info->config = cfg;
                info->status = AgentStatus::unloaded;
                registry_[cfg.name] = std::move(info);
                std::cerr << "[supervisor] Loaded agent: " << cfg.name << "\n";
            }
            ++loaded;
        } catch (const std::exception& e) {
            std::cerr << "[supervisor] Failed to load "
                      << entry.path().filename() << ": " << e.what() << "\n";
        }
    }

    return loaded;
}

// ── Run ───────────────────────────────────────────────────────────────────────
//
// Parallel execution design:
//   1. Hold registry_mutex_ briefly to get config + increment counters.
//   2. Release lock — all expensive work (LLM, MCP, file I/O) happens unlocked.
//   3. Reacquire lock briefly at the end to update status/error.
//
// Multiple requests — even for the same agent — execute concurrently.
// AgentMemory uses flock() to safely merge concurrent history writes.

std::string Supervisor::run_agent(const std::string& name,
                                  const std::string& message,
                                  bool verbose) {
    // ── Step 1: get config and mark as running (brief lock) ───────────────────
    std::shared_ptr<AgentInfo> info;
    AgentConfig cfg;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = registry_.find(name);
        if (it == registry_.end())
            throw std::runtime_error("Unknown agent: " + name);
        info = it->second;
        cfg  = info->config;
        ++info->run_count;
        ++info->active_runs;
        info->status = AgentStatus::running;
    }

    // ── Step 2: load memory (unlocked — file I/O) ─────────────────────────────
    const char* turns_env = std::getenv("AGENTOS_MEMORY_TURNS");
    int mem_turns = turns_env ? std::stoi(turns_env) : 10;

    AgentMemory mem(cfg.name);
    auto history = mem.load(mem_turns);

    // ── Step 3: run a fresh AgentInstance (unlocked — LLM inference) ──────────
    std::string llm_url = cfg.llm_url.empty() ? default_llm_url_ : cfg.llm_url;
    std::string api_key = cfg.llm_api_key.empty() ? default_api_key_ : cfg.llm_api_key;

    std::string response;
    try {
        AgentInstance instance(cfg, agentos_server_url_, llm_url, api_key, history);
        response = verbose
            ? instance.run_verbose(message)
            : instance.run(message);

        // Save turn to memory (unlocked — flock-protected internally)
        mem.save_turn(message, response);

        // ── Step 4a: update status on success (brief lock) ────────────────────
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            --info->active_runs;
            info->status     = (info->active_runs > 0)
                                   ? AgentStatus::running
                                   : AgentStatus::idle;
            info->last_error = "";
        }
    } catch (const std::exception& e) {
        // ── Step 4b: update status on failure (brief lock) ────────────────────
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            --info->active_runs;
            ++info->error_count;
            info->status     = AgentStatus::error;
            info->last_error = e.what();
        }
        throw std::runtime_error("Agent '" + name + "' failed: " + e.what());
    }

    return response;
}

// ── Memory clear ──────────────────────────────────────────────────────────────

void Supervisor::clear_memory(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        if (!registry_.count(name))
            throw std::runtime_error("Unknown agent: " + name);
    }
    AgentMemory mem(name);
    mem.clear();
    std::cerr << "[supervisor] Cleared memory for agent: " << name << "\n";
}

// ── Listing ───────────────────────────────────────────────────────────────────

std::string Supervisor::status_str(AgentStatus s) {
    switch (s) {
        case AgentStatus::idle:     return "idle";
        case AgentStatus::running:  return "running";
        case AgentStatus::error:    return "error";
        case AgentStatus::unloaded: return "unloaded";
    }
    return "unknown";
}

std::vector<Supervisor::AgentSnapshot> Supervisor::list_agents() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::vector<AgentSnapshot> result;
    for (const auto& [name, info] : registry_) {
        AgentSnapshot snap;
        snap.name        = name;
        snap.description = info->config.description;
        snap.model       = info->config.model;
        snap.status      = status_str(info->status);
        snap.last_error  = info->last_error;
        snap.run_count   = info->run_count;
        snap.error_count = info->error_count;
        snap.active_runs = info->active_runs;
        snap.priority    = static_cast<int>(info->config.priority);
        result.push_back(snap);
    }
    return result;
}

Supervisor::AgentSnapshot Supervisor::agent_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = registry_.find(name);
    if (it == registry_.end()) return {};
    const auto& info = it->second;
    AgentSnapshot snap;
    snap.name        = name;
    snap.description = info->config.description;
    snap.model       = info->config.model;
    snap.status      = status_str(info->status);
    snap.last_error  = info->last_error;
    snap.run_count   = info->run_count;
    snap.error_count = info->error_count;
    snap.active_runs = info->active_runs;
    snap.priority    = static_cast<int>(info->config.priority);
    return snap;
}
