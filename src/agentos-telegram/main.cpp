// =============================================================================
// src/agentos-telegram/main.cpp — agentos-telegram: Telegram bot for AgentOS
// =============================================================================
//
// Telegram bot that polls the Telegram Bot API and routes messages to 
// agentos-supervisor.  Runs as a standalone long-polling service.
//
// Usage:
//   agentos-telegram
//
// Environment:
//   TELEGRAM_BOT_TOKEN           (required — exit with error if missing)
//   AGENTOS_SUPERVISOR_URL       (default: http://localhost:8889)
//   AGENTOS_DEFAULT_AGENT        (default: sysmonitor)
//
// Architecture:
//   - Polls Telegram API with getUpdates (30s timeout, offset-based)
//   - Routes messages to agentos-supervisor via HTTP
//   - Sends responses back to Telegram via sendMessage
//
// Message routing:
//   /start              → intro message
//   /help               → list commands
//   /agents             → list available agents
//   /run agent_name msg → run named agent with message
//   /memory clear name  → clear agent memory (not yet implemented)
//   plain text          → run default agent
//
// =============================================================================

#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <string>
#include <regex>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sstream>

using json = nlohmann::json;

// ── Configuration ─────────────────────────────────────────────────────────────

static const char* getenv_or(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return v ? v : fallback;
}

struct Config {
    std::string telegram_bot_token;
    std::string supervisor_url;
    std::string default_agent;
};

static Config load_config() {
    const char* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!token || !token[0]) {
        std::cerr << "[agentos-telegram] ERROR: TELEGRAM_BOT_TOKEN not set\n";
        std::exit(1);
    }
    return {
        token,
        getenv_or("AGENTOS_SUPERVISOR_URL", "http://localhost:8889"),
        getenv_or("AGENTOS_DEFAULT_AGENT", "sysmonitor")
    };
}

// ── URL parsing helper ─────────────────────────────────────────────────────────

struct ServerConn {
    std::string host;
    int port;
};

static ServerConn parse_url(const std::string& url) {
    std::regex re(R"(https?://([^/:]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(url, m, re))
        return {"localhost", 8889};
    return {m[1].str(), m[2].matched ? std::stoi(m[2].str()) : 8889};
}

// ── Logging helpers ────────────────────────────────────────────────────────────

static void log_error(const std::string& msg) {
    std::cerr << "[agentos-telegram] " << msg << "\n";
}

static void log_info(const std::string& msg) {
    std::cerr << "[agentos-telegram] " << msg << "\n";
}

// ── String utilities ───────────────────────────────────────────────────────────

static std::string truncate(const std::string& s, size_t max_len) {
    if (s.length() <= max_len) return s;
    return s.substr(0, max_len) + "...";
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        result.push_back(item);
    }
    return result;
}

static std::string ltrim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r\f\v");
    return (start == std::string::npos) ? "" : s.substr(start);
}

// ── Telegram API helpers ───────────────────────────────────────────────────────

static bool telegram_send_message(const Config& cfg, int64_t chat_id, const std::string& text) {
    httplib::SSLClient tg_cli("api.telegram.org", 443);
    tg_cli.enable_server_certificate_verification(false); // dev environment
    tg_cli.set_read_timeout(35, 0); // 35 seconds for poll timeout + margin

    json payload;
    payload["chat_id"] = chat_id;
    payload["text"] = text;

    std::string path = "/bot" + cfg.telegram_bot_token + "/sendMessage";
    auto res = tg_cli.Post(path, payload.dump(), "application/json");

    if (!res) {
        log_error("Telegram sendMessage failed: no response from api.telegram.org");
        return false;
    }
    if (res->status != 200) {
        log_error("Telegram sendMessage failed: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

// ── Supervisor API helpers ─────────────────────────────────────────────────────

static json supervisor_get_agents(const Config& cfg) {
    auto [host, port] = parse_url(cfg.supervisor_url);
    httplib::Client cli(host, port);
    cli.set_read_timeout(30, 0);

    auto res = cli.Get("/agents");
    if (!res) {
        throw std::runtime_error("Cannot reach supervisor: " + cfg.supervisor_url);
    }
    if (res->status != 200) {
        throw std::runtime_error("Supervisor /agents returned HTTP " + std::to_string(res->status));
    }
    return json::parse(res->body);
}

static json supervisor_run_agent(const Config& cfg, const std::string& agent_name, const std::string& message) {
    auto [host, port] = parse_url(cfg.supervisor_url);
    httplib::Client cli(host, port);
    cli.set_read_timeout(300, 0); // 300 seconds for LLM responses

    json body;
    body["message"] = message;

    std::string path = "/agents/" + agent_name + "/run";
    auto res = cli.Post(path, body.dump(), "application/json");

    if (!res) {
        throw std::runtime_error("Cannot reach supervisor at " + cfg.supervisor_url);
    }
    if (res->status != 200) {
        std::string error_msg = res->body;
        if (res->status == 404) {
            error_msg = "Agent '" + agent_name + "' not found";
        }
        throw std::runtime_error("Agent '" + agent_name + "' error: " + error_msg);
    }
    return json::parse(res->body);
}

// ── Message parsing and routing ────────────────────────────────────────────────

static std::string handle_start_command() {
    return "Welcome to AgentOS Telegram Bot!\n\n"
           "I can help you interact with your AI agents via Telegram.\n\n"
           "Use /help to see available commands.";
}

static std::string handle_help_command() {
    return "Available commands:\n\n"
           "/start             - Show welcome message\n"
           "/help              - Show this help message\n"
           "/agents            - List all available agents\n"
           "/run agent_name msg - Run an agent with a message\n"
           "/memory clear name - Clear agent memory (not yet implemented)\n\n"
           "Or just send plain text to run the default agent.";
}

static std::string handle_agents_command(const Config& cfg) {
    try {
        auto agents = supervisor_get_agents(cfg);
        std::string result = "Available agents:\n\n";
        
        if (agents.is_array()) {
            for (size_t i = 0; i < agents.size(); ++i) {
                const auto& agent = agents[i];
                std::string name = agent.contains("name") ? agent["name"].get<std::string>() : "unknown";
                result += "• " + name + "\n";
            }
        } else if (agents.is_object() && agents.contains("agents")) {
            auto agent_list = agents["agents"];
            if (agent_list.is_array()) {
                for (const auto& agent : agent_list) {
                    std::string name = agent.is_string() ? agent.get<std::string>() : agent.dump();
                    result += "• " + name + "\n";
                }
            }
        }
        
        if (result == "Available agents:\n\n") {
            result += "(none found)";
        }
        return result;
    } catch (const std::exception& e) {
        return "Error fetching agents: " + std::string(e.what());
    }
}

static std::string handle_run_agent_command(const Config& cfg, const std::string& args) {
    // Parse: /run agent_name message text here
    std::vector<std::string> parts = split(args, ' ');
    if (parts.size() < 2) {
        return "Usage: /run agent_name message";
    }
    
    std::string agent_name = parts[0];
    
    // Reconstruct message from remaining parts
    std::string message;
    for (size_t i = 1; i < parts.size(); ++i) {
        if (!message.empty()) message += " ";
        message += parts[i];
    }
    
    try {
        auto result = supervisor_run_agent(cfg, agent_name, message);
        std::string response = result.is_string() ? result.get<std::string>() : result.dump();
        log_info("Agent '" + agent_name + "' response length: " + std::to_string(response.length()));
        return response;
    } catch (const std::exception& e) {
        return "Error: " + std::string(e.what());
    }
}

static std::string handle_memory_clear_command(const std::string& args) {
    // Parse: /memory clear agent_name
    std::vector<std::string> parts = split(args, ' ');
    if (parts.size() < 2 || parts[0] != "clear") {
        return "Usage: /memory clear agent_name";
    }
    return "Memory clear is not yet implemented";
}

static std::string handle_user_message(const Config& cfg, const std::string& text) {
    // Check for commands
    if (text.substr(0, 6) == "/start") {
        return handle_start_command();
    }
    if (text.substr(0, 5) == "/help") {
        return handle_help_command();
    }
    if (text.substr(0, 7) == "/agents") {
        return handle_agents_command(cfg);
    }
    if (text.substr(0, 4) == "/run") {
        std::string args = ltrim(text.substr(4));
        return handle_run_agent_command(cfg, args);
    }
    if (text.substr(0, 7) == "/memory") {
        std::string args = ltrim(text.substr(7));
        return handle_memory_clear_command(args);
    }
    
    // Plain text: run default agent
    try {
        auto result = supervisor_run_agent(cfg, cfg.default_agent, text);
        std::string response = result.is_string() ? result.get<std::string>() : result.dump();
        log_info("Default agent '" + cfg.default_agent + "' response length: " + std::to_string(response.length()));
        return response;
    } catch (const std::exception& e) {
        if (std::string(e.what()).find("Cannot reach supervisor") != std::string::npos) {
            return "Sorry, AgentOS supervisor is not running.";
        }
        return "Error: " + std::string(e.what());
    }
}

// ── Telegram polling loop ──────────────────────────────────────────────────────

static void run_polling_loop(const Config& cfg) {
    httplib::SSLClient tg_cli("api.telegram.org", 443);
    tg_cli.enable_server_certificate_verification(false); // dev environment
    tg_cli.set_read_timeout(35, 0); // 30s poll timeout + 5s margin

    int offset = 0;
    log_info("Starting long-polling loop (timeout=30s)");

    while (true) {
        try {
            std::string path = "/bot" + cfg.telegram_bot_token + "/getUpdates?timeout=30&offset=" + std::to_string(offset);
            auto res = tg_cli.Get(path);

            if (!res) {
                log_error("Telegram getUpdates: no response from api.telegram.org");
                std::cerr.flush();
                continue;
            }

            if (res->status != 200) {
                log_error("Telegram getUpdates returned HTTP " + std::to_string(res->status));
                std::cerr.flush();
                continue;
            }

            json updates = json::parse(res->body);
            if (!updates.contains("ok") || !updates["ok"].get<bool>()) {
                log_error("Telegram returned ok=false");
                std::cerr.flush();
                continue;
            }

            if (!updates.contains("result")) {
                continue;
            }

            auto result = updates["result"];
            if (!result.is_array()) {
                continue;
            }

            for (const auto& update : result) {
                if (!update.contains("update_id")) {
                    continue;
                }

                int update_id = update["update_id"].get<int>();
                offset = update_id + 1;

                // Check if this is a message update
                if (!update.contains("message")) {
                    continue;
                }

                const auto& msg = update["message"];
                if (!msg.contains("chat") || !msg.contains("text")) {
                    continue;
                }

                int64_t chat_id = msg["chat"]["id"].get<int64_t>();
                std::string text = msg["text"].get<std::string>();
                std::string username = "unknown";
                if (msg.contains("from") && msg["from"].contains("username")) {
                    username = msg["from"]["username"].get<std::string>();
                }

                // Log incoming message
                log_info("Message from @" + username + " (chat " + std::to_string(chat_id) + "): " + truncate(text, 80));

                // Handle the message
                std::string response = handle_user_message(cfg, text);

                // Send response back to Telegram
                if (!telegram_send_message(cfg, chat_id, response)) {
                    log_error("Failed to send response to chat " + std::to_string(chat_id));
                }
            }

            std::cerr.flush();

        } catch (const std::exception& e) {
            log_error("Exception in polling loop: " + std::string(e.what()));
            std::cerr.flush();
        }
    }
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    Config cfg = load_config();
    log_info("Telegram bot initialized with supervisor: " + cfg.supervisor_url);
    log_info("Default agent: " + cfg.default_agent);

    run_polling_loop(cfg);
    return 0;
}
