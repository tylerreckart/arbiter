#pragma once
// index/include/constitution.h — Constitution system
// Master constitution (caveman-derived) + per-agent personality overlays.

#include <string>
#include <vector>
#include <optional>

namespace index_ai {

// Caveman compression level
enum class Brevity { Lite, Full, Ultra };

struct Constitution {
    // --- Core identity ---
    std::string name;
    std::string role;               // e.g. "code-reviewer", "researcher", "devops"
    std::string personality;        // free-form personality overlay

    // --- Behavioral rules ---
    Brevity brevity = Brevity::Full;
    int     max_tokens = 1024;      // response cap
    double  temperature = 0.3;      // low = deterministic
    std::string model = "claude-sonnet-4-latest";

    // Agent mode — selects the base system prompt.
    // ""/"standard": compressed index voice (default for all agents)
    // "writer": full-prose mode — disables compression, enables writing guidance
    std::string mode;

    // Optional advisor model (beta: advisor-tool-2026-03-01).
    // When set, the executor model can consult this higher-intelligence model
    // mid-generation for strategic planning. Must be >= capability of executor.
    // Example: model="claude-haiku-4-5-20251001", advisor_model="claude-opus-4-6"
    std::string advisor_model;  // "" = disabled

    // --- System prompt pieces ---
    std::string goal;               // what this agent is trying to accomplish
    std::vector<std::string> rules; // explicit behavioral constraints

    // --- Routing signal ---
    // Tools this agent is designed to use.  Shown in the master's roster so
    // index can route based on capability rather than inferring from goal text.
    // Example: {"/fetch", "/mem"} for researcher, {"/exec", "/write"} for devops.
    std::vector<std::string> capabilities;

    // --- Computed ---
    std::string build_system_prompt() const;

    // --- Serialization ---
    std::string to_json() const;
    static Constitution from_json(const std::string& json_str);
    static Constitution from_file(const std::string& path);
    void save(const std::string& path) const;
};

// Master constitution — caveman-derived defaults
Constitution master_constitution();

std::string brevity_to_string(Brevity b);
Brevity brevity_from_string(const std::string& s);

} // namespace index_ai
