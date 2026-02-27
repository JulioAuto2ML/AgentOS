// =============================================================================
// src/agent/agent_memory.h — Persistent per-agent conversation history
// =============================================================================
//
// AgentMemory stores a rolling log of (user, assistant) turn pairs for each
// agent. On each run the supervisor loads the recent history and passes it
// to the AgentInstance, which prepends it after the system prompt so the
// model has context from previous sessions.
//
// Storage:  $AGENTOS_MEMORY_DIR/<agent_name>.json
//           Default: ~/.agentos/memory/<agent_name>.json
//
// Format:   JSON array of {"user": "...", "assistant": "..."} objects.
//           Only user/assistant pairs are stored — system prompts and tool
//           call details are excluded to keep memory compact.
//
// Env vars:
//   AGENTOS_MEMORY_DIR    Override storage directory
//   AGENTOS_MEMORY_TURNS  Max past turns to load per run (default: 10)
//
// Thread safety: save_turn() acquires a file lock so parallel runs of the
//               same agent don't corrupt each other's history.
// =============================================================================

#pragma once
#include "llm_client.h"
#include <string>
#include <vector>

class AgentMemory {
public:
    explicit AgentMemory(const std::string& agent_name);

    // Load last max_turns (user+assistant) pairs from disk as ChatMessages,
    // ready to be inserted into the conversation after the system prompt.
    // Returns an empty vector if no history file exists yet.
    std::vector<ChatMessage> load(int max_turns = 10) const;

    // Append one completed turn to disk.
    // Uses flock() so concurrent agents can write safely.
    void save_turn(const std::string& user_msg,
                   const std::string& assistant_response);

    // Delete all stored history for this agent.
    void clear();

    // Return the configured memory directory (reads env or uses default).
    static std::string memory_dir();

private:
    std::string path_;  // full path to <agent_name>.json
};
