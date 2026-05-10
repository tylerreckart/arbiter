#pragma once
// arbiter/include/constitution.h — Constitution system
// Master constitution (caveman-derived) + per-agent personality overlays.

#include <string>
#include <vector>
#include <optional>

namespace arbiter {

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
    //
    // Legacy field — kept for back-compat with existing agent JSON.  New
    // configurations should populate `advisor` (below); the parser mirrors
    // `advisor_model` into `advisor.model` with mode="consult" when only the
    // legacy field is set.
    std::string advisor_model;  // "" = disabled

    // Advisor — structured runtime supervision.  Two modes:
    //   "consult"  (default when only advisor_model is set): /advise slash
    //              command works; advisor is consultative, executor decides
    //              if/when to ask.
    //   "gate"     advisor is invoked by the runtime on each terminating
    //              executor turn and emits CONTINUE / REDIRECT / HALT.  The
    //              executor cannot return a final result without CONTINUE.
    //   "off"      advisor disabled entirely; /advise returns ERR.
    struct AdvisorConfig {
        std::string model;             // required when mode != "off"
        std::string prompt;            // optional gate-prompt override
        std::string mode = "consult";  // "off" | "consult" | "gate"
        int  max_redirects = 2;        // per top-level turn
        bool malformed_halts = true;   // gate fails closed by default
    } advisor;

    // Memory enrichment toggles.  Control whether /mem search and
    // /mem add entry invoke advisor-driven enhancements that improve
    // retrieval quality at the cost of one extra LLM call per
    // operation.  Defaults preserve current behavior (no extra calls)
    // for agents that haven't opted in; agents whose work depends on
    // long-running memory (research, planner) typically want all on.
    //
    // `intent_routing` is the exception — it's a regex-based question
    // classifier with zero LLM cost, defaulted on because the worst
    // case is "no boost applied" (monotonic vs off).
    struct MemoryConfig {
        // /mem search query reformulation.  When true, the orchestrator
        // calls the agent's advisor model once per search to generate
        // 2 paraphrases, runs each through FTS, RRF-fuses the
        // rankings.  Closes the recall gap on paraphrased queries
        // without any embedding storage.  Cost: ~150ms + ~$0.0001 per
        // search.
        bool search_expand    = false;

        // /mem add entry auto-tagging.  When true, an advisor extracts
        // 2-4 topical tags from title+content before the row is
        // written.  Tags get the existing 8x BM25 weight on retrieval,
        // so this is one of the strongest no-cost signals for future
        // /mem search calls — most agent ingest paths leave tags
        // empty otherwise.
        bool auto_tag         = false;

        // /mem add entry auto-supersession.  When true, after the new
        // entry is created the advisor inspects the top-K FTS hits on
        // the same title, decides whether the new entry directly
        // contradicts any of them, and invalidates the contradicted
        // ones.  Closes the knowledge-update accuracy gap where users
        // change their minds without explicitly invalidating the old
        // fact.  Bias is toward "leave alone" — false positives erase
        // legitimate prior memory.
        bool auto_supersede   = false;

        // /mem search intent routing.  Heuristic question classifier
        // (no LLM call) maps cue words ("favorite", "when", "how to")
        // to memory-entry type boosts via the existing 1.3x BM25
        // multiplier.  Defaults on because the worst case is "no
        // match, no boost."
        bool intent_routing   = true;

        // /mem search age-decay.  When true, BM25 scores are scaled by
        // a piecewise factor of the entry's age — fresh entries keep
        // their score, week-old entries take a small hit, year-old
        // entries float at `age_floor`.  Recall doesn't collapse (the
        // floor is a multiplier > 0), but ranking biases toward fresh
        // evidence.  Defaults on; the floor of 0.5 keeps old entries
        // discoverable for queries that have no fresher match.
        bool   age_decay         = true;
        int    age_half_life_days = 90;   // 30 → 0.9, 90 → 0.75, 180 → 0.6 …
        double age_floor         = 0.5;
    } memory;

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

} // namespace arbiter
