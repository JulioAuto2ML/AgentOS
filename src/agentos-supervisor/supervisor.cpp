// =============================================================================
// src/agentos-supervisor/supervisor.cpp
// =============================================================================

#include "supervisor.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

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
                // Update config but keep existing instance if status is idle
                registry_[cfg.name]->config = cfg;
                std::cerr << "[supervisor] Reloaded agent: " << cfg.name << "\n";
            } else {
                auto info        = std::make_shared<AgentInfo>();
                info->config     = cfg;
                info->status     = AgentStatus::unloaded;
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

// ── Instance management ───────────────────────────────────────────────────────

void Supervisor::ensure_instance(AgentInfo& info) {
    // Called with info.run_mutex held (exclusive via unique_lock in run_agent)
    if (!info.instance) {
        info.instance = std::make_unique<AgentInstance>(
            info.config,
            agentos_server_url_,
            info.config.llm_url.empty() ? default_llm_url_ : info.config.llm_url,
            info.config.llm_api_key.empty() ? default_api_key_ : info.config.llm_api_key
        );
        info.status = AgentStatus::idle;
    }
}

// ── Run ───────────────────────────────────────────────────────────────────────

std::string Supervisor::run_agent(const std::string& name,
                                  const std::string& message,
                                  bool verbose) {
    std::shared_ptr<AgentInfo> info;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = registry_.find(name);
        if (it == registry_.end())
            throw std::runtime_error("Unknown agent: " + name);
        info = it->second;
    }

    // Acquire per-agent mutex: queues concurrent requests naturally
    std::unique_lock<std::mutex> run_lock(info->run_mutex);

    ensure_instance(*info);

    info->status = AgentStatus::running;
    ++info->run_count;

    std::string response;
    try {
        response = verbose
            ? info->instance->run_verbose(message)
            : info->instance->run(message);
        info->status     = AgentStatus::idle;
        info->last_error = "";
    } catch (const std::exception& e) {
        info->status     = AgentStatus::error;
        info->last_error = e.what();
        ++info->error_count;
        // Reset instance so next call reconnects cleanly
        info->instance.reset();
        throw std::runtime_error("Agent '" + name + "' failed: " + e.what());
    }

    return response;
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
    snap.priority    = static_cast<int>(info->config.priority);
    return snap;
}
