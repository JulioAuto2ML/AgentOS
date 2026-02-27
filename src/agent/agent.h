// =============================================================================
// src/agent/agent.h — AgentInstance: runs an LLM agent with MCP tool access
// =============================================================================
//
// An AgentInstance ties together:
//   - AgentConfig (from YAML): name, model, tools allowlist, system prompt
//   - LLMClient: sends chat completions to the configured LLM backend
//   - MCP tool loop: calls agentos-server tools and feeds results back to the LLM
//
// Typical usage:
//
//   AgentConfig cfg = AgentConfig::from_file("agents/sysmonitor.yaml");
//   AgentInstance agent(cfg, "http://localhost:8888");  // agentos-server URL
//   std::string answer = agent.run("What processes are using the most RAM?");
//   std::cout << answer << "\n";
//
// The agent iterates up to cfg.max_steps rounds of:
//   LLM call → tool call → append result → repeat
// until the model produces a plain text response (no tool calls).
// =============================================================================

#pragma once
#include "agent_config.h"
#include "llm_client.h"
#include "mcp_sse_client.h"
#include <string>
#include <vector>
#include <memory>

class AgentInstance {
public:
    // agentos_server_url: base URL of agentos-server, e.g. "http://localhost:8888"
    // initial_history:    past (user, assistant) turns loaded from AgentMemory,
    //                     prepended after the system prompt for cross-run memory.
    AgentInstance(const AgentConfig&             cfg,
                  const std::string&              agentos_server_url,
                  const std::string&              default_llm_url    = "http://localhost:8080",
                  const std::string&              default_api_key    = "",
                  const std::vector<ChatMessage>& initial_history    = {});

    ~AgentInstance();

    // Run the agent with a user message. Returns the final text response.
    // Throws std::runtime_error on LLM or MCP errors.
    std::string run(const std::string& user_message);

    // Same as run() but emits intermediate steps to stderr for debugging.
    std::string run_verbose(const std::string& user_message);

    const AgentConfig& config() const { return cfg_; }

private:
    AgentConfig                      cfg_;
    LLMClient                        llm_;
    std::unique_ptr<mcp::sse_client> mcp_;            // MCP session with agentos-server
    json                             tools_schema_;   // OpenAI-format tool defs
    std::vector<ChatMessage>         initial_history_; // past turns from AgentMemory

    // Connect to agentos-server, fetch tools, build tools_schema_
    void connect_and_fetch_tools(const std::string& host, int port);

    // Call a single tool via the MCP session
    json call_tool(const std::string& name, const json& arguments);

    // Convert mcp::tool to OpenAI function schema
    static json mcp_tool_to_openai(const mcp::tool& t);

    // Core loop shared by run() and run_verbose()
    std::string run_loop(const std::string& user_message, bool verbose);
};
