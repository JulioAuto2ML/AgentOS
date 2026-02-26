// =============================================================================
// src/agent/agent.cpp — AgentInstance implementation
// =============================================================================
//
// Tool loop design:
//
//   1. On construction, fetch the list of tools from nos-server (tools/list).
//      Filter to the agent's allowlist (if non-empty) and build the OpenAI
//      tool schema array.
//
//   2. run_loop() builds the initial conversation:
//        [system_prompt, user_message]
//      then enters the loop:
//
//        a. Send messages + tools_schema to LLM → CompletionResponse
//        b. If response has no tool calls → return response.content (done)
//        c. For each tool call:
//             - Call nos-server via MCP HTTP POST
//             - Append assistant message (with tool_calls) to history
//             - Append tool result message to history
//        d. If step count >= max_steps → return error message
//        e. Go to (a)
//
// MCP tool call (HTTP, no SSE session needed for single calls):
//   POST /message?session_id=<agent-name>
//   Body: JSON-RPC 2.0 tools/call request
//   The server returns the result synchronously in the HTTP response body.
//
// Note: nos-server currently echoes the result directly in the POST response
// body (not via SSE), which is why we can use a simple HTTP call here.
// =============================================================================

#include "agent.h"
#include "httplib.h"
#include <iostream>
#include <stdexcept>
#include <regex>

// ── Constructor ───────────────────────────────────────────────────────────────

AgentInstance::AgentInstance(const AgentConfig&  cfg,
                             const std::string&  nos_server_url,
                             const std::string&  default_llm_url,
                             const std::string&  default_api_key)
    : cfg_(cfg)
    , llm_(cfg.llm_url.empty() ? default_llm_url : cfg.llm_url,
           cfg.llm_api_key.empty() ? default_api_key : cfg.llm_api_key,
           cfg.model == "default" ? "llama3.1" : cfg.model)
    , nos_server_url_(nos_server_url)
{
    llm_.set_max_tokens(cfg.context_limit / 4); // conservative: 1/4 of context for output
    fetch_tools();
}

// ── URL parsing helpers ───────────────────────────────────────────────────────

static std::pair<std::string, int> parse_host_port(const std::string& url) {
    std::regex re(R"(^https?://([^/:]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(url, m, re))
        throw std::runtime_error("Invalid nos-server URL: " + url);
    std::string host = m[1].str();
    int port = m[2].matched ? std::stoi(m[2].str()) : 8888;
    return {host, port};
}

// ── MCP tool call ─────────────────────────────────────────────────────────────

json AgentInstance::call_tool(const std::string& name, const json& arguments) {
    auto [host, port] = parse_host_port(nos_server_url_);
    httplib::Client cli(host, port);
    cli.set_read_timeout(30);

    const std::string session = "agent-" + cfg_.name;

    json rpc = {
        {"jsonrpc", "2.0"},
        {"id",      1},
        {"method",  "tools/call"},
        {"params",  {{"name", name}, {"arguments", arguments}}}
    };

    auto result = cli.Post(
        "/message?session_id=" + session,
        rpc.dump(),
        "application/json"
    );

    if (!result)
        throw std::runtime_error("nos-server unreachable: " +
            std::string(httplib::to_string(result.error())));

    if (result->status != 200 && result->status != 202)
        throw std::runtime_error("nos-server error " +
            std::to_string(result->status) + ": " + result->body);

    // Response may be empty (202 Accepted) — result comes via SSE.
    // For simple single calls, nos-server echoes result in body.
    if (result->body.empty()) return {{"result", "accepted"}};

    try {
        json resp = json::parse(result->body);
        if (resp.contains("result"))
            return resp["result"];
        if (resp.contains("error"))
            throw std::runtime_error("Tool error: " + resp["error"].dump());
        return resp;
    } catch (const json::exception& e) {
        // Body might not be JSON (e.g. empty 202) — return raw
        return {{"raw", result->body}};
    }
}

// ── Tool schema fetch ─────────────────────────────────────────────────────────

json AgentInstance::mcp_tool_to_openai(const json& mcp_tool) {
    // MCP tool format: {name, description, inputSchema: {type, properties, required}}
    // OpenAI format:   {type: "function", function: {name, description, parameters}}
    json params = mcp_tool.contains("inputSchema")
        ? mcp_tool["inputSchema"]
        : json{{"type", "object"}, {"properties", json::object()}};

    return {
        {"type", "function"},
        {"function", {
            {"name",        mcp_tool["name"]},
            {"description", mcp_tool.value("description", "")},
            {"parameters",  params}
        }}
    };
}

void AgentInstance::fetch_tools() {
    auto [host, port] = parse_host_port(nos_server_url_);
    httplib::Client cli(host, port);
    cli.set_read_timeout(10);

    const std::string session = "agent-" + cfg_.name + "-init";
    json rpc = {
        {"jsonrpc", "2.0"},
        {"id",      1},
        {"method",  "tools/list"},
        {"params",  json::object()}
    };

    auto result = cli.Post(
        "/message?session_id=" + session,
        rpc.dump(),
        "application/json"
    );

    if (!result || (result->status != 200 && result->status != 202)) {
        // nos-server not reachable — continue without tools
        tools_schema_ = json::array();
        return;
    }

    try {
        json resp = json::parse(result->body);
        json tools_list = resp.contains("result") && resp["result"].contains("tools")
            ? resp["result"]["tools"]
            : json::array();

        tools_schema_ = json::array();
        for (const auto& tool : tools_list) {
            const std::string tool_name = tool["name"].get<std::string>();

            // Apply allowlist filter
            if (!cfg_.tools.empty()) {
                bool allowed = false;
                for (const auto& t : cfg_.tools)
                    if (t == tool_name) { allowed = true; break; }
                if (!allowed) continue;
            }

            tools_schema_.push_back(mcp_tool_to_openai(tool));
        }
    } catch (...) {
        tools_schema_ = json::array(); // fallback
    }
}

// ── Core inference loop ───────────────────────────────────────────────────────

std::string AgentInstance::run_loop(const std::string& user_message, bool verbose) {
    std::vector<ChatMessage> history;

    // System prompt
    if (!cfg_.system_prompt.empty()) {
        ChatMessage sys; sys.role = "system"; sys.content = cfg_.system_prompt;
        history.push_back(std::move(sys));
    }

    // User turn
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
        // each tool result message. We store the tool_calls array directly
        // in ChatMessage::tool_calls so build_messages_json serialises it
        // correctly.
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
                // Extract text content if it's MCP format
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
                          << tc.name << "): " << tool_result.substr(0, 120) << "...\n";

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
