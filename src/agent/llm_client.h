// =============================================================================
// src/agent/llm_client.h — OpenAI-compatible LLM HTTP client
// =============================================================================
//
// Sends chat completion requests to any OpenAI-compatible endpoint:
//   - llama-server (llama.cpp) at http://localhost:8080
//   - Groq at https://api.groq.com/openai
//   - Anthropic (via their OpenAI-compat endpoint)
//   - Any other server implementing POST /v1/chat/completions
//
// Supports the OpenAI tool-calling format so agents can request tool
// invocations from the LLM.
//
// Usage:
//   LLMClient client("http://localhost:8080", "", "llama3.1");
//   auto resp = client.complete(messages, tools);
//   if (!resp.tool_calls.empty()) { /* handle tool calls */ }
//   else { /* resp.content is the final answer */ }
// =============================================================================

#pragma once
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

// ── Data structures ───────────────────────────────────────────────────────────

struct ChatMessage {
    std::string role;           // "system" | "user" | "assistant" | "tool"
    std::string content;
    std::string tool_call_id;   // set when role == "tool" (result of a tool call)
    std::string name;           // set when role == "tool" (tool name)
    json        tool_calls;     // set when role == "assistant" and model called tools
                                // (json::array of OpenAI-format tool call objects)
};

struct ToolCall {
    std::string id;             // unique call ID from the model
    std::string name;           // tool name
    json        arguments;      // parsed arguments object
};

struct CompletionResponse {
    std::string           content;      // final text (empty if tool_calls non-empty)
    std::vector<ToolCall> tool_calls;   // requested tool invocations
    int                   prompt_tokens     = 0;
    int                   completion_tokens = 0;
};

// ── LLMClient ─────────────────────────────────────────────────────────────────

class LLMClient {
public:
    // base_url: e.g. "http://localhost:8080" or "https://api.groq.com/openai"
    // api_key:  Bearer token (empty for local llama-server)
    // model:    model identifier, e.g. "llama3.1", "llama-3.1-8b-instant"
    LLMClient(const std::string& base_url,
              const std::string& api_key,
              const std::string& model);

    // Send a chat completion request.
    // tools_schema: array of OpenAI-format tool definitions (may be empty).
    // Returns: model's response (text or tool calls).
    // Throws: std::runtime_error on HTTP or JSON errors.
    CompletionResponse complete(
        const std::vector<ChatMessage>& messages,
        const json& tools_schema = json::array()
    );

    void set_temperature(float t)   { temperature_ = t; }
    void set_max_tokens(int n)      { max_tokens_  = n; }

private:
    std::string host_;      // e.g. "localhost" or "api.groq.com"
    int         port_;      // e.g. 8080, 443
    bool        https_;     // true if scheme is https
    std::string base_path_; // e.g. "" or "/openai"
    std::string api_key_;
    std::string model_;
    float       temperature_ = 0.0f;
    int         max_tokens_  = 1024;

    // Parse base_url into (host_, port_, https_, base_path_)
    void parse_url(const std::string& url);

    // Build the messages JSON array from ChatMessage vector
    json build_messages_json(const std::vector<ChatMessage>& messages);
};
