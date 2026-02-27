// =============================================================================
// src/agent/agent_memory.cpp — Persistent per-agent conversation history
// =============================================================================

#include "agent_memory.h"
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Directory / path ──────────────────────────────────────────────────────────

std::string AgentMemory::memory_dir() {
    const char* env = std::getenv("AGENTOS_MEMORY_DIR");
    if (env && *env) return env;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.agentos/memory";
}

AgentMemory::AgentMemory(const std::string& agent_name) {
    std::string dir = memory_dir();
    fs::create_directories(dir);          // no-op if already exists
    path_ = dir + "/" + agent_name + ".json";
}

// ── Load ──────────────────────────────────────────────────────────────────────

std::vector<ChatMessage> AgentMemory::load(int max_turns) const {
    std::vector<ChatMessage> result;
    if (!fs::exists(path_)) return result;

    try {
        std::ifstream f(path_);
        json arr = json::parse(f);
        if (!arr.is_array()) return result;

        // Take only the last max_turns entries (rolling window)
        int start = std::max(0, static_cast<int>(arr.size()) - max_turns);
        for (int i = start; i < static_cast<int>(arr.size()); ++i) {
            const auto& turn = arr[i];
            ChatMessage u; u.role = "user";      u.content = turn["user"].get<std::string>();
            ChatMessage a; a.role = "assistant"; a.content = turn["assistant"].get<std::string>();
            result.push_back(std::move(u));
            result.push_back(std::move(a));
        }
    } catch (...) {
        // Corrupt or empty file — silently return empty history
    }
    return result;
}

// ── Save ──────────────────────────────────────────────────────────────────────

void AgentMemory::save_turn(const std::string& user_msg,
                             const std::string& assistant_response) {
    // Acquire exclusive file lock to prevent concurrent write corruption
    std::string lock_path = path_ + ".lock";
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    // Read current history
    json arr = json::array();
    if (fs::exists(path_)) {
        try {
            std::ifstream f(path_);
            arr = json::parse(f);
            if (!arr.is_array()) arr = json::array();
        } catch (...) {
            arr = json::array();
        }
    }

    // Append new turn
    arr.push_back({{"user", user_msg}, {"assistant", assistant_response}});

    // Atomic write: write to .tmp first, then rename
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp);
        f << arr.dump(2);
    }
    fs::rename(tmp, path_);

    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

// ── Clear ─────────────────────────────────────────────────────────────────────

void AgentMemory::clear() {
    if (fs::exists(path_)) fs::remove(path_);
}
