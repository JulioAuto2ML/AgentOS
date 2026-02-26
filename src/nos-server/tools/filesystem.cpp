#include "tools.h"
#include "mcp_message.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

// ── read_file ────────────────────────────────────────────────────────────────

static mcp::json read_file_handler(const mcp::json& params, const std::string&) {
    if (!params.contains("path") || !params["path"].is_string())
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'path'");

    fs::path path = params["path"].get<std::string>();

    if (!fs::exists(path))
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            "File not found: " + path.string());
    if (!fs::is_regular_file(path))
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            "Not a regular file: " + path.string());

    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw mcp::mcp_exception(mcp::error_code::internal_error,
            "Cannot open file: " + path.string());

    const long offset = params.value("offset", 0);
    const long limit  = params.value("limit",  0); // 0 = no limit

    if (offset > 0)
        file.seekg(offset);

    std::ostringstream ss;
    if (limit > 0) {
        std::string buf(static_cast<size_t>(limit), '\0');
        file.read(buf.data(), limit);
        buf.resize(static_cast<size_t>(file.gcount()));
        ss << buf;
    } else {
        ss << file.rdbuf();
    }

    mcp::json result = {
        {"path",    path.string()},
        {"content", ss.str()},
        {"size",    static_cast<long long>(fs::file_size(path))}
    };

    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

// ── write_file ───────────────────────────────────────────────────────────────

static mcp::json write_file_handler(const mcp::json& params, const std::string&) {
    if (!params.contains("path") || !params["path"].is_string())
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'path'");
    if (!params.contains("content") || !params["content"].is_string())
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'content'");

    fs::path    path    = params["path"].get<std::string>();
    std::string content = params["content"].get<std::string>();
    bool        append  = params.value("append", false);

    // Create parent dirs if needed
    if (path.has_parent_path())
        fs::create_directories(path.parent_path());

    auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
    std::ofstream file(path, mode);
    if (!file)
        throw mcp::mcp_exception(mcp::error_code::internal_error,
            "Cannot write file: " + path.string());

    file << content;
    file.flush();

    mcp::json result = {
        {"path",          path.string()},
        {"bytes_written", static_cast<long long>(content.size())},
        {"appended",      append}
    };

    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

// ── list_dir ─────────────────────────────────────────────────────────────────

static mcp::json list_dir_handler(const mcp::json& params, const std::string&) {
    if (!params.contains("path") || !params["path"].is_string())
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'path'");

    fs::path path = params["path"].get<std::string>();

    if (!fs::exists(path))
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            "Path not found: " + path.string());
    if (!fs::is_directory(path))
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            "Not a directory: " + path.string());

    mcp::json entries = mcp::json::array();
    for (const auto& entry : fs::directory_iterator(path)) {
        std::string type = entry.is_directory()   ? "dir"
                         : entry.is_regular_file() ? "file"
                         : entry.is_symlink()       ? "symlink"
                         : "other";
        long long size = entry.is_regular_file() ? static_cast<long long>(entry.file_size()) : -1;
        entries.push_back({
            {"name", entry.path().filename().string()},
            {"type", type},
            {"size", size}
        });
    }

    mcp::json result = {{"path", path.string()}, {"entries", entries}};
    return mcp::json::array({{{"type", "text"}, {"text", result.dump()}}});
}

// ── registration ─────────────────────────────────────────────────────────────

void register_filesystem_tools(mcp::server& server) {
    auto read_tool = mcp::tool_builder("read_file")
        .with_description("Read the contents of a file. Supports offset and byte limit.")
        .with_string_param("path",   "Absolute or relative path to the file")
        .with_number_param("offset", "Byte offset to start reading from (default: 0)", false)
        .with_number_param("limit",  "Maximum bytes to read (default: 0 = all)", false)
        .build();

    auto write_tool = mcp::tool_builder("write_file")
        .with_description("Write content to a file. Creates parent directories if needed.")
        .with_string_param("path",    "Absolute or relative path to the file")
        .with_string_param("content", "Text content to write")
        .with_boolean_param("append", "Append instead of overwrite (default: false)", false)
        .build();

    auto list_tool = mcp::tool_builder("list_dir")
        .with_description("List the contents of a directory.")
        .with_string_param("path", "Path to the directory")
        .build();

    server.register_tool(read_tool,  read_file_handler);
    server.register_tool(write_tool, write_file_handler);
    server.register_tool(list_tool,  list_dir_handler);
}
