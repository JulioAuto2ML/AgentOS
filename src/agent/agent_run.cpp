// =============================================================================
// src/agent/agent_run.cpp — nos-agent-run: run an agent from the command line
// =============================================================================
//
// A minimal CLI for running a single agent turn. Useful for testing agent
// YAML files and the inference loop without the full nos-supervisor.
//
// Usage:
//   nos-agent-run <agent.yaml> [message]
//   nos-agent-run <agent.yaml> --message "What is the CPU usage?"
//
// If message is omitted, reads from stdin (one line).
//
// Environment variables:
//   NOS_LLM_URL     LLM backend URL (default: http://localhost:8080)
//   NOS_LLM_KEY     API key for the LLM (default: empty)
//   NOS_SERVER_URL  nos-server URL (default: http://localhost:8888)
//   NOS_VERBOSE     Set to "1" to print intermediate tool calls to stderr
//
// Examples:
//   # Run sysmonitor agent with Groq backend
//   NOS_LLM_URL=https://api.groq.com/openai \
//   NOS_LLM_KEY=gsk_... \
//   nos-agent-run agents/sysmonitor.yaml "What's eating my RAM?"
//
//   # Run hello agent locally (llama-server must be running on :8080)
//   nos-agent-run agents/hello.yaml "Who are you?"
// =============================================================================

#include "agent_config.h"
#include "agent.h"
#include <iostream>
#include <string>
#include <cstdlib>

static const char* getenv_or(const char* var, const char* fallback) {
    const char* val = std::getenv(var);
    return val ? val : fallback;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <agent.yaml> [message]\n"
                  << "       " << argv[0] << " <agent.yaml> --message \"text\"\n"
                  << "\nEnvironment:\n"
                  << "  NOS_LLM_URL     LLM backend (default: http://localhost:8080)\n"
                  << "  NOS_LLM_KEY     API key (default: empty)\n"
                  << "  NOS_SERVER_URL  nos-server URL (default: http://localhost:8888)\n"
                  << "  NOS_VERBOSE     '1' for verbose output\n";
        return 1;
    }

    const std::string yaml_path      = argv[1];
    const std::string llm_url        = getenv_or("NOS_LLM_URL",    "http://localhost:8080");
    const std::string llm_key        = getenv_or("NOS_LLM_KEY",    "");
    const std::string llm_model      = getenv_or("NOS_LLM_MODEL",  "");  // empty = use agent YAML
    const std::string nos_server_url = getenv_or("NOS_SERVER_URL", "http://localhost:8888");
    const bool        verbose        = std::string(getenv_or("NOS_VERBOSE", "0")) == "1";

    // Parse message from args or stdin
    std::string message;
    if (argc >= 3) {
        std::string flag = argv[2];
        if (flag == "--message" && argc >= 4) {
            message = argv[3];
        } else if (flag != "--message") {
            // Remaining args joined as the message
            for (int i = 2; i < argc; ++i) {
                if (i > 2) message += " ";
                message += argv[i];
            }
        }
    }

    if (message.empty()) {
        if (!std::cin.eof()) {
            std::cerr << "Enter message (Ctrl-D when done):\n";
            std::string line;
            while (std::getline(std::cin, line)) {
                if (!message.empty()) message += "\n";
                message += line;
            }
        }
    }

    if (message.empty()) {
        std::cerr << "Error: no message provided\n";
        return 1;
    }

    // Load agent and run
    AgentConfig cfg;
    try {
        cfg = AgentConfig::from_file(yaml_path);
    } catch (const std::exception& e) {
        std::cerr << "Error loading agent: " << e.what() << "\n";
        return 1;
    }

    // NOS_LLM_MODEL overrides the model in the YAML (useful for testing different models)
    if (!llm_model.empty()) cfg.model = llm_model;

    // "default" means: let the server decide (llama-server ignores the field;
    // for Groq you must set NOS_LLM_MODEL or put the model name in the YAML)
    const std::string display_model = (cfg.model == "default") ? "(server default)" : cfg.model;

    std::cerr << "[nos-agent-run] Agent: " << cfg.name
              << " | LLM: " << llm_url
              << " | Model: " << display_model
              << "\n";

    try {
        AgentInstance agent(cfg, nos_server_url, llm_url, llm_key);
        std::string response = verbose
            ? agent.run_verbose(message)
            : agent.run(message);
        std::cout << response << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
