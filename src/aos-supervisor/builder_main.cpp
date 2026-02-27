// =============================================================================
// src/aos-supervisor/builder_main.cpp — aos-builder CLI
// =============================================================================
//
// A thin wrapper around aos-agent-run that always uses the builder agent.
// Provides friendlier UX: shows the generated YAML after creation.
//
// Usage:
//   aos-builder "Create an agent that monitors disk usage"
//   aos-builder --verbose "Create a network monitor agent"
//
// Environment: same as aos-agent-run (AOS_LLM_URL, AOS_LLM_KEY, etc.)
//
// Internally: calls the builder agent defined in agents/builder.yaml,
// which uses write_file to create the new agent YAML.
// After the agent responds, we scan agents/ for newly created files
// and print their contents for review.
// =============================================================================

#include "../agent/agent_config.h"
#include "../agent/agent.h"
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <chrono>

namespace fs = std::filesystem;

static const char* getenv_or(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return v ? v : fallback;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    std::string description;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else {
            if (!description.empty()) description += " ";
            description += arg;
        }
    }

    if (description.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--verbose] \"describe the agent you want\"\n"
                  << "\nExamples:\n"
                  << "  " << argv[0] << " \"Monitor disk usage and alert when above 80%\"\n"
                  << "  " << argv[0] << " \"Watch nginx logs for 5xx errors\"\n"
                  << "  " << argv[0] << " \"List the top memory consumers every minute\"\n";
        return 1;
    }

    const std::string llm_url     = getenv_or("AOS_LLM_URL",    "http://localhost:8080");
    const std::string llm_key     = getenv_or("AOS_LLM_KEY",    "");
    const std::string llm_model   = getenv_or("AOS_LLM_MODEL",  "");
    const std::string server_url  = getenv_or("AOS_SERVER_URL", "http://localhost:8888");
    const std::string agents_dir  = getenv_or("AOS_AGENTS_DIR", "./agents");

    // Snapshot of existing agents/ files before builder runs
    std::set<std::string> before;
    if (fs::exists(agents_dir)) {
        for (const auto& e : fs::directory_iterator(agents_dir))
            if (e.path().extension() == ".yaml" || e.path().extension() == ".yml")
                before.insert(e.path().string());
    }

    // Load builder agent
    const std::string builder_yaml = std::string(agents_dir) + "/builder.yaml";
    AgentConfig cfg;
    try {
        cfg = AgentConfig::from_file(builder_yaml);
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot load builder agent (" << builder_yaml << "): "
                  << e.what() << "\n";
        return 1;
    }

    if (!llm_model.empty()) cfg.model = llm_model;

    std::cerr << "[aos-builder] Asking builder agent to create: \""
              << description << "\"\n";

    // Run the builder agent
    std::string response;
    try {
        AgentInstance agent(cfg, server_url, llm_url, llm_key);
        response = verbose ? agent.run_verbose(description) : agent.run(description);
    } catch (const std::exception& e) {
        std::cerr << "Error running builder agent: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n" << response << "\n";

    // Show any newly created YAML files
    std::vector<std::string> new_files;
    if (fs::exists(agents_dir)) {
        for (const auto& e : fs::directory_iterator(agents_dir)) {
            if ((e.path().extension() == ".yaml" || e.path().extension() == ".yml")
                && !before.count(e.path().string()))
                new_files.push_back(e.path().string());
        }
    }

    for (const auto& path : new_files) {
        std::cerr << "\n── " << path << " ──────────────────────────────\n";
        std::cerr << read_file(path) << "\n";
        std::cerr << "── (end of " << fs::path(path).filename().string() << ") ─\n";
    }

    if (!new_files.empty()) {
        std::cerr << "\n[aos-builder] To activate: "
                  << "curl -s -X POST http://localhost:8889/agents/reload "
                  << "-H 'Content-Type: application/json' -d '{}'\n";
    }

    return 0;
}
