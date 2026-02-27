// =============================================================================
// tools/script_tools.cpp — Dynamic script-based MCP tool loader
// =============================================================================

#include "script_tools.h"
#include "mcp_message.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <cstdio>
#include <array>
#include <sys/wait.h>
#include <iostream>

namespace fs = std::filesystem;

// ── Script metadata ───────────────────────────────────────────────────────────

struct ScriptParam {
    std::string name;
    std::string type;        // string | number | boolean | integer
    bool        required;
    std::string description;
};

struct ScriptMeta {
    std::string path;        // absolute path to the script
    std::string name;
    std::string description;
    std::vector<ScriptParam> params;
    bool valid = false;
};

// ── Metadata parser ───────────────────────────────────────────────────────────

// Parse @tool directives from the leading comment block of a script file.
// Only reads lines that start with '#' (stops at first non-comment, non-empty line).
static ScriptMeta parse_script_meta(const std::string& path) {
    ScriptMeta meta;
    meta.path = path;

    std::ifstream f(path);
    if (!f) return meta;

    // @tool name:        <value>
    // @tool description: <value>
    // @tool param:       <name> <type> <required|optional> <description...>
    static const std::regex re_name (R"(#\s*@tool\s+name\s*:\s*(.+))");
    static const std::regex re_desc (R"(#\s*@tool\s+description\s*:\s*(.+))");
    static const std::regex re_param(R"(#\s*@tool\s+param\s*:\s*(\S+)\s+(\S+)\s+(required|optional)\s+(.*))");

    std::string line;
    while (std::getline(f, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t'))
            line.pop_back();

        if (line.empty()) continue;
        if (line[0] != '#') break;  // end of header comment block

        std::smatch m;
        if (std::regex_match(line, m, re_name)) {
            meta.name = m[1];
            // trim leading spaces
            auto p = meta.name.find_first_not_of(' ');
            if (p != std::string::npos) meta.name = meta.name.substr(p);
        } else if (std::regex_match(line, m, re_desc)) {
            meta.description = m[1];
            auto p = meta.description.find_first_not_of(' ');
            if (p != std::string::npos) meta.description = meta.description.substr(p);
        } else if (std::regex_match(line, m, re_param)) {
            ScriptParam param;
            param.name        = m[1];
            param.type        = m[2];
            param.required    = (m[3] == "required");
            param.description = m[4];
            meta.params.push_back(std::move(param));
        }
    }

    meta.valid = !meta.name.empty() && !meta.description.empty();
    return meta;
}

// ── Script executor ───────────────────────────────────────────────────────────

// Run the script, piping JSON params to stdin. Returns stdout as text content.
static mcp::json run_script(const std::string& path, const mcp::json& params) {
    // Determine interpreter
    bool is_python = (path.size() >= 3 &&
                      path.substr(path.size() - 3) == ".py");
    std::string interpreter = is_python ? "python3" : "bash";

    // Build command: echo '{"key":"val"}' | python3 /path/to/script.py
    // We write params JSON to a temp pipe via popen with stdin redirection.
    // Approach: use /bin/sh -c 'echo JSON | interpreter script'
    std::string params_str = params.dump();

    // Shell-quote the JSON string (single-quote it, escape embedded single-quotes)
    std::string quoted;
    quoted.reserve(params_str.size() + 2);
    quoted += '\'';
    for (char c : params_str) {
        if (c == '\'') quoted += "'\\''";
        else           quoted += c;
    }
    quoted += '\'';

    std::string cmd = "echo " + quoted + " | " + interpreter + " " + path + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        throw mcp::mcp_exception(mcp::error_code::internal_error,
                                  "Failed to execute script: " + path);

    std::ostringstream output;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), buf.size(), pipe))
        output << buf.data();

    int raw_status = pclose(pipe);
    int exit_code  = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;

    std::string result = output.str();
    if (exit_code != 0) {
        result = "[exit " + std::to_string(exit_code) + "]\n" + result;
    }

    return mcp::json::array({{
        {"type", "text"},
        {"text", result}
    }});
}

// ── Tool registration ─────────────────────────────────────────────────────────

static void register_one_script(mcp::server& server, const ScriptMeta& meta) {
    auto builder = mcp::tool_builder(meta.name)
        .with_description(meta.description);

    for (const auto& p : meta.params) {
        if (p.type == "string") {
            builder.with_string_param(p.name, p.description, p.required);
        } else if (p.type == "number") {
            builder.with_number_param(p.name, p.description, p.required);
        } else if (p.type == "integer") {
            builder.with_number_param(p.name, p.description, p.required);
        } else if (p.type == "boolean") {
            builder.with_boolean_param(p.name, p.description, p.required);
        } else {
            // Unknown type — fall back to string
            builder.with_string_param(p.name, p.description, p.required);
        }
    }

    mcp::tool tool = builder.build();
    std::string script_path = meta.path;  // capture by value for lambda

    server.register_tool(tool, [script_path](const mcp::json& params, const std::string&) {
        return run_script(script_path, params);
    });
}

void register_script_tools(mcp::server& server, const std::string& tools_dir) {
    // Prefer AGENTOS_TOOLS_DIR env var if set
    const char* env_dir = std::getenv("AGENTOS_TOOLS_DIR");
    const std::string dir = (env_dir && *env_dir) ? std::string(env_dir) : tools_dir;

    if (!fs::exists(dir)) {
        std::cerr << "[agentos-server] Script tools dir not found: " << dir << "\n";
        return;
    }

    int count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string path = entry.path().string();
        const std::string ext  = entry.path().extension().string();

        if (ext != ".py" && ext != ".sh") continue;

        ScriptMeta meta = parse_script_meta(path);
        if (!meta.valid) {
            std::cerr << "[agentos-server] Skipping " << path
                      << " (missing @tool name/description)\n";
            continue;
        }

        try {
            register_one_script(server, meta);
            std::cerr << "[agentos-server] Loaded script tool: " << meta.name
                      << " (" << entry.path().filename().string() << ")\n";
            ++count;
        } catch (const std::exception& e) {
            std::cerr << "[agentos-server] Failed to register " << path
                      << ": " << e.what() << "\n";
        }
    }

    std::cerr << "[agentos-server] Script tools loaded: " << count << "\n";
}
