// =============================================================================
// src/agent/agent.cpp — AgentInstance implementation
// =============================================================================
//
// Tool loop design:
//
//   1. On construction, connect to agentos-server via MCP SSE session.
//      The SSE client (mcp::sse_client) handles the /sse handshake and
//      session management required by the MCP protocol before any
//      tool calls can be made.
//
//   2. Fetch tool list via mcp::sse_client::get_tools(), filter to the
//      agent's allowlist, and convert to OpenAI function schema format.
//
//   3. run_loop() builds the initial conversation:
//        [system_prompt, user_message]
//      then enters the loop:
//
//        a. Send messages + tools_schema to LLM → CompletionResponse
//        b. If response has no tool calls → return response.content (done)
//        c. For each tool call:
//             - Call agentos-server via mcp_->call_tool(name, args)
//             - Append assistant message (with tool_calls) to history
//             - Append tool result message to history
//        d. If step count >= max_steps → return error message
//        e. Go to (a)
// =============================================================================

#include "agent.h"
#include <iostream>
#include <stdexcept>
#include <regex>

// ── URL parsing ───────────────────────────────────────────────────────────────

static std::pair<std::string, int> parse_agentos_url(const std::string& url) {
    // Match http://host[:port] — HTTPS not needed for local agentos-server
    std::regex re(R"(^https?://([^/:]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(url, m, re))
        throw std::runtime_error("Invalid agentos-server URL: " + url);
    std::string host = m[1].str();
    int port = m[2].matched ? std::stoi(m[2].str()) : 8888;
    return {host, port};
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

AgentInstance::AgentInstance(const AgentConfig&             cfg,
                             const std::string&              agentos_server_url,
                             const std::string&              default_llm_url,
                             const std::string&              default_api_key,
                             const std::vector<ChatMessage>& initial_history)
    : cfg_(cfg)
    , llm_(cfg.llm_url.empty() ? default_llm_url : cfg.llm_url,
           cfg.llm_api_key.empty() ? default_api_key : cfg.llm_api_key,
           cfg.model)  // "default" is passed through; llama-server ignores the field
    , initial_history_(initial_history)
{
    llm_.set_max_tokens(cfg.context_limit / 4);

    auto [host, port] = parse_agentos_url(agentos_server_url);
    connect_and_fetch_tools(host, port);
}

AgentInstance::~AgentInstance() = default;

// ── MCP connection and tool schema ────────────────────────────────────────────

json AgentInstance::mcp_tool_to_openai(const mcp::tool& t) {
    // mcp::tool::to_json() returns {name, description, inputSchema: {...}}
    // We need OpenAI format: {type: "function", function: {name, description, parameters}}
    json tj = t.to_json();
    json params = tj.contains("inputSchema")
        ? tj["inputSchema"]
        : json{{"type", "object"}, {"properties", json::object()}};

    return {
        {"type", "function"},
        {"function", {
            {"name",        t.name},
            {"description", t.description},
            {"parameters",  params}
        }}
    };
}

void AgentInstance::connect_and_fetch_tools(const std::string& host, int port) {
    // Create SSE client and establish MCP session
    mcp_ = std::make_unique<mcp::sse_client>(host, port);

    if (!mcp_->initialize("agentos-agent-" + cfg_.name, "0.1.0")) {
        // agentos-server not reachable — proceed with empty tool list
        // (agent can still work as a pure LLM without tools)
        tools_schema_ = json::array();
        mcp_.reset();
        return;
    }

    // Get all tools from agentos-server
    std::vector<mcp::tool> all_tools = mcp_->get_tools();

    tools_schema_ = json::array();
    for (const auto& tool : all_tools) {
        // Apply allowlist: if cfg_.tools is non-empty, only include listed tools
        if (!cfg_.tools.empty()) {
            bool allowed = false;
            for (const auto& allowed_name : cfg_.tools)
                if (allowed_name == tool.name) { allowed = true; break; }
            if (!allowed) continue;
        }
        tools_schema_.push_back(mcp_tool_to_openai(tool));
    }
}

// ── Tool call ─────────────────────────────────────────────────────────────────

json AgentInstance::call_tool(const std::string& name, const json& arguments) {
    if (!mcp_)
        throw std::runtime_error("No MCP session (agentos-server unreachable)");

    // mcp::sse_client::call_tool returns the raw MCP result JSON
    return mcp_->call_tool(name, arguments);
}

// ── Core inference loop ───────────────────────────────────────────────────────

std::string AgentInstance::run_loop(const std::string& user_message, bool verbose) {
    std::vector<ChatMessage> history;

    // System prompt
    if (!cfg_.system_prompt.empty()) {
        ChatMessage sys; sys.role = "system"; sys.content = cfg_.system_prompt;
        history.push_back(std::move(sys));
    }

    // Memory: prepend past (user, assistant) turns after system prompt so the
    // model has context from previous sessions without seeing raw tool calls.
    for (const auto& msg : initial_history_)
        history.push_back(msg);

    // Current user turn
    ChatMessage user_msg; user_msg.role = "user"; user_msg.content = user_message;
    history.push_back(std::move(user_msg));

    for (int step = 0; step < cfg_.max_steps; ++step) {
        if (verbose)
            std::cerr << "[agent:" << cfg_.name << "] LLM call (step " << step+1 << ")\n";

        CompletionResponse resp = llm_.complete(history, tools_schema_);

        // No tool calls → final response
        if (resp.tool_calls.empty()) {
            if (verbose)
                std::cerr << "[agent:" << cfg_.name << "] Done. Tokens used: "
                          << resp.prompt_tokens + resp.completion_tokens << "\n";
            return resp.content;
        }

        // Build assistant message with tool_calls for history.
        // OpenAI requires: assistant message (with tool_calls array) BEFORE
        // each tool result message.
        json tc_arr = json::array();
        for (const auto& tc : resp.tool_calls) {
            tc_arr.push_back({
                {"id",   tc.id},
                {"type", "function"},
                {"function", {
                    {"name",      tc.name},
                    {"arguments", tc.arguments.dump()}
                }}
            });
        }
        ChatMessage asst_msg;
        asst_msg.role        = "assistant";
        asst_msg.content     = resp.content;  // may be empty when tool_calls present
        asst_msg.tool_calls  = tc_arr;
        history.push_back(std::move(asst_msg));

        // Execute each tool call
        for (const auto& tc : resp.tool_calls) {
            if (verbose)
                std::cerr << "[agent:" << cfg_.name << "] Tool call: "
                          << tc.name << "(" << tc.arguments.dump() << ")\n";

            std::string tool_result;
            try {
                json result = call_tool(tc.name, tc.arguments);
                // MCP result format: {content: [{type: "text", text: "..."}]}
                if (result.contains("content") && result["content"].is_array()
                    && !result["content"].empty()) {
                    tool_result = result["content"][0].value("text", result.dump());
                } else {
                    tool_result = result.dump();
                }
            } catch (const std::exception& e) {
                tool_result = "{\"error\": \"" + std::string(e.what()) + "\"}";
            }

            if (verbose)
                std::cerr << "[agent:" << cfg_.name << "] Tool result ("
                          << tc.name << "): "
                          << tool_result.substr(0, 120) << "...\n";

            ChatMessage tool_msg;
            tool_msg.role         = "tool";
            tool_msg.content      = tool_result;
            tool_msg.tool_call_id = tc.id;
            tool_msg.name         = tc.name;
            history.push_back(std::move(tool_msg));
        }
    }

    return "[Agent '" + cfg_.name + "' reached max_steps=" +
           std::to_string(cfg_.max_steps) + " without a final response]";
}

// ── Public interface ──────────────────────────────────────────────────────────

std::string AgentInstance::run(const std::string& user_message) {
    return run_loop(user_message, false);
}

std::string AgentInstance::run_verbose(const std::string& user_message) {
    return run_loop(user_message, true);
}
