// =============================================================================
// src/agent/agent_config.cpp — YAML parsing for AgentConfig
// =============================================================================
//
// Parses agent definition files (.yaml) into AgentConfig structs.
//
// Expected YAML schema (all optional except `name`):
//
//   name: my-agent
//   description: What this agent does
//   model: llama3.1:8b          # or "default"
//   llm_url: http://localhost:8080
//   llm_api_key: sk-...
//   tools:                       # allowlist; empty = all tools
//     - sysinfo
//     - exec
//   system_prompt: |
//     You are a helpful agent...
//   priority: normal             # low | normal | high | realtime
//   context_limit: 4096
//   max_steps: 10
//
// Uses yaml-cpp 0.7 (libyaml-cpp-dev). Build with:
//   find_package(yaml-cpp REQUIRED) + target_link_libraries(... yaml-cpp)
// =============================================================================

#include "agent_config.h"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

// ── helpers ───────────────────────────────────────────────────────────────────

static AgentPriority parse_priority(const std::string& s) {
    if (s == "low")      return AgentPriority::low;
    if (s == "high")     return AgentPriority::high;
    if (s == "realtime") return AgentPriority::realtime;
    return AgentPriority::normal;
}

static AgentConfig from_node(const YAML::Node& root, const std::string& source) {
    if (!root["name"])
        throw std::runtime_error("Agent YAML missing required field 'name' in: " + source);

    AgentConfig cfg;
    cfg.name        = root["name"].as<std::string>();
    cfg.description = root["description"] ? root["description"].as<std::string>() : "";
    cfg.model       = root["model"]       ? root["model"].as<std::string>()       : "default";
    cfg.llm_url     = root["llm_url"]     ? root["llm_url"].as<std::string>()     : "";
    cfg.llm_api_key = root["llm_api_key"] ? root["llm_api_key"].as<std::string>() : "";
    cfg.system_prompt = root["system_prompt"] ? root["system_prompt"].as<std::string>() : "";

    if (root["priority"])
        cfg.priority = parse_priority(root["priority"].as<std::string>());
    if (root["context_limit"])
        cfg.context_limit = root["context_limit"].as<int>();
    if (root["max_steps"])
        cfg.max_steps = root["max_steps"].as<int>();

    if (root["tools"] && root["tools"].IsSequence()) {
        for (const auto& t : root["tools"])
            cfg.tools.push_back(t.as<std::string>());
    }

    return cfg;
}

// ── factory ───────────────────────────────────────────────────────────────────

AgentConfig AgentConfig::from_file(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse agent YAML '" + path + "': " + e.what());
    }
    return from_node(root, path);
}

AgentConfig AgentConfig::from_string(const std::string& yaml_content) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_content);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse agent YAML string: " + std::string(e.what()));
    }
    return from_node(root, "<string>");
}
