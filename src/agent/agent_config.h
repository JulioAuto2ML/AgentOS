#pragma once
#include <string>
#include <vector>
#include <stdexcept>

// ── AgentConfig ───────────────────────────────────────────────────────────────
// Parsed representation of an agent definition file (.yaml).
// See agents/ for examples.

enum class AgentPriority { low, normal, high, realtime };

struct AgentConfig {
    std::string name;
    std::string description;

    // LLM backend — if llm_url is empty, the supervisor's default is used.
    std::string model     = "default";
    std::string llm_url;
    std::string llm_api_key;

    // Which nos-server tools this agent is allowed to call.
    // Empty list = all available tools.
    std::vector<std::string> tools;

    std::string system_prompt;

    AgentPriority priority    = AgentPriority::normal;
    int           context_limit = 4096;
    int           max_steps     = 10;   // max tool-call rounds per turn

    // ── Factory ───────────────────────────────────────────────────────────────
    static AgentConfig from_file(const std::string& path);
    static AgentConfig from_string(const std::string& yaml_content);
};

inline std::string to_string(AgentPriority p) {
    switch (p) {
        case AgentPriority::low:      return "low";
        case AgentPriority::normal:   return "normal";
        case AgentPriority::high:     return "high";
        case AgentPriority::realtime: return "realtime";
    }
    return "normal";
}
