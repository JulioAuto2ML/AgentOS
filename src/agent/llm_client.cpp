// =============================================================================
// src/agent/llm_client.cpp — OpenAI-compatible LLM HTTP client (implementation)
// =============================================================================
//
// Uses httplib (already vendored in third-party/cpp-mcp/common/httplib.h) for
// synchronous HTTP/HTTPS POST requests to /v1/chat/completions.
//
// Wire format (OpenAI chat completions API):
//
// Request:
//   POST /v1/chat/completions
//   Content-Type: application/json
//   Authorization: Bearer <api_key>
//
//   {
//     "model": "llama3.1",
//     "messages": [
//       {"role": "system", "content": "..."},
//       {"role": "user",   "content": "..."}
//     ],
//     "tools": [...],          // optional tool definitions
//     "tool_choice": "auto",   // optional, "auto" lets model decide
//     "temperature": 0.0,
//     "max_tokens": 1024
//   }
//
// Response:
//   {
//     "choices": [{
//       "message": {
//         "role": "assistant",
//         "content": "text OR null",
//         "tool_calls": [{    // present if model wants to call a tool
//           "id": "call_abc",
//           "type": "function",
//           "function": {
//             "name": "sysinfo",
//             "arguments": "{}"
//           }
//         }]
//       }
//     }],
//     "usage": {"prompt_tokens": 100, "completion_tokens": 50}
//   }
// =============================================================================

// CPPHTTPLIB_OPENSSL_SUPPORT is defined globally via CMake (target_compile_definitions
// on the mcp target) so that ALL translation units see the same httplib::Client layout.
// Defining it here would cause a "redefined" warning and, more importantly, if it were
// ever missing in one TU it would cause heap corruption (ODR violation).
#include "llm_client.h"
#include "httplib.h"
#include <stdexcept>
#include <sstream>
#include <iostream>
#include <regex>

// ── URL parsing ───────────────────────────────────────────────────────────────

void LLMClient::parse_url(const std::string& url) {
    // Match: (http|https)://host[:port][/path]
    std::regex re(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re))
        throw std::runtime_error("Invalid LLM URL: " + url);

    https_     = (m[1].str() == "https");
    host_      = m[2].str();
    port_      = m[3].matched ? std::stoi(m[3].str()) : (https_ ? 443 : 80);
    base_path_ = m[4].matched ? m[4].str() : "";

    // Remove trailing slash from base_path_
    if (!base_path_.empty() && base_path_.back() == '/')
        base_path_.pop_back();
}

// ── Constructor ───────────────────────────────────────────────────────────────

LLMClient::LLMClient(const std::string& base_url,
                     const std::string& api_key,
                     const std::string& model)
    : api_key_(api_key), model_(model)
{
    parse_url(base_url);
}

// ── Message serialisation ─────────────────────────────────────────────────────

json LLMClient::build_messages_json(const std::vector<ChatMessage>& messages) {
    json arr = json::array();
    for (const auto& msg : messages) {
        json m = {{"role", msg.role}};

        // "tool" role: result of a tool call — needs tool_call_id and name
        if (msg.role == "tool") {
            m["content"]      = msg.content;
            m["tool_call_id"] = msg.tool_call_id;
            m["name"]         = msg.name;
        }
        // "assistant" with tool calls: content may be null per OpenAI spec
        else if (msg.role == "assistant"
                 && !msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            m["content"]    = msg.content.empty() ? json(nullptr) : json(msg.content);
            m["tool_calls"] = msg.tool_calls;
        }
        // "system" | "user" | plain "assistant"
        else {
            m["content"] = msg.content;
        }

        arr.push_back(m);
    }
    return arr;
}

// ── Main request ──────────────────────────────────────────────────────────────

CompletionResponse LLMClient::complete(
    const std::vector<ChatMessage>& messages,
    const json& tools_schema)
{
    // Build request body
    json body = {
        {"model",       model_},
        {"messages",    build_messages_json(messages)},
        {"temperature", temperature_},
        {"max_tokens",  max_tokens_}
    };

    if (!tools_schema.empty()) {
        body["tools"]       = tools_schema;
        body["tool_choice"] = "auto";
    }

    const std::string path = base_path_ + "/v1/chat/completions";
    const std::string body_str = body.dump();

    // Set up headers
    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "application/json"}
    };
    if (!api_key_.empty())
        headers.emplace("Authorization", "Bearer " + api_key_);

    // Make HTTP(S) request
    httplib::Result result;
    if (https_) {
        httplib::SSLClient cli(host_, port_);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(120);       // LLMs can be slow
        cli.enable_server_certificate_verification(true);
        result = cli.Post(path, headers, body_str, "application/json");
    } else {
        httplib::Client cli(host_, port_);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(120);
        result = cli.Post(path, headers, body_str, "application/json");
    }

    if (!result)
        throw std::runtime_error("HTTP request failed: " +
            std::string(httplib::to_string(result.error())));

    if (result->status != 200)
        throw std::runtime_error("LLM API error " +
            std::to_string(result->status) + ": " + result->body);

    // Parse response
    json resp;
    try {
        resp = json::parse(result->body);
    } catch (const json::exception& e) {
        throw std::runtime_error("Invalid JSON from LLM: " + std::string(e.what()));
    }

    CompletionResponse out;

    // Usage
    if (resp.contains("usage")) {
        out.prompt_tokens     = resp["usage"].value("prompt_tokens",     0);
        out.completion_tokens = resp["usage"].value("completion_tokens", 0);
    }

    // First choice
    if (!resp.contains("choices") || resp["choices"].empty())
        throw std::runtime_error("LLM response has no choices");

    const json& msg = resp["choices"][0]["message"];

    // Check for tool calls
    if (msg.contains("tool_calls") && !msg["tool_calls"].is_null()) {
        for (const auto& tc : msg["tool_calls"]) {
            ToolCall call;
            call.id   = tc.value("id", "");
            call.name = tc["function"]["name"].get<std::string>();
            const std::string args_str = tc["function"]["arguments"].get<std::string>();
            try {
                call.arguments = json::parse(args_str);
            } catch (...) {
                call.arguments = json::object(); // fallback if model returns invalid JSON
            }
            out.tool_calls.push_back(std::move(call));
        }
    } else {
        // Plain text response
        out.content = msg.value("content", "");
        if (out.content.empty() && msg["content"].is_null())
            out.content = "";
    }

    return out;
}
