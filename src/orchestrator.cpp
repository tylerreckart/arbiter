// arbiter/src/orchestrator.cpp
#include "orchestrator.h"
#include "commands.h"
#include "config.h"
#include "tui/stream_filter.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace arbiter {

namespace {

// ─── Thread-local streaming context ─────────────────────────────────────────
//
// Each in-flight agent turn carries (stream_id, agent_id, depth).  A turn
// runs on exactly one thread — sequential delegation reuses the request
// thread; /parallel spawns fresh threads, one per child.  Thread-local
// storage lets any callback invoked DURING the turn (text stream,
// tool_status, cost, etc.) read the current context with zero coordination
// — no mutex, no parameter threading.
//
// The parent's context is stashed on the RAII guard's stack frame so
// sequential calls naturally restore the parent's id on return.  Parallel
// children start in a fresh thread where the thread-local defaults to zero,
// and the child's StreamScope sets it before any callback fires.
thread_local int         tl_stream_id    = 0;
thread_local std::string tl_stream_agent;
thread_local int         tl_stream_depth = 0;

struct StreamScope {
    int         prev_id;
    std::string prev_agent;
    int         prev_depth;
    StreamScope(int id, const std::string& agent, int depth)
        : prev_id(tl_stream_id),
          prev_agent(tl_stream_agent),
          prev_depth(tl_stream_depth) {
        tl_stream_id    = id;
        tl_stream_agent = agent;
        tl_stream_depth = depth;
    }
    ~StreamScope() {
        tl_stream_id    = prev_id;
        tl_stream_agent = std::move(prev_agent);
        tl_stream_depth = prev_depth;
    }
    StreamScope(const StreamScope&) = delete;
    StreamScope& operator=(const StreamScope&) = delete;
};

// Master-agent model resolution order (first match wins):
//   1. ~/.arbiter/master_model  — explicit user choice (set by the setup wizard
//      or hand-edited).  One line, trimmed.
//   2. An available-provider default based on which API keys are configured
//      — Anthropic wins, OpenAI is the fallback.
//   3. The hardcoded default baked into master_constitution().
// The master can still be retargeted at runtime via `/model index <id>`;
// that change is session-scoped and does not touch the override file.
std::string load_master_model_override() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return {};
    std::ifstream f(std::string(home) + "/.arbiter/master_model");
    if (!f) return {};
    std::string m;
    std::getline(f, m);
    while (!m.empty() &&
           std::isspace(static_cast<unsigned char>(m.back()))) m.pop_back();
    return m;
}

std::string pick_master_model_default(
    const std::map<std::string, std::string>& keys) {
    if (keys.count("anthropic")) return "claude-sonnet-4-6";
    if (keys.count("openai"))    return "openai/gpt-4.1";
    return {};
}

}  // namespace

Orchestrator::Orchestrator(std::map<std::string, std::string> api_keys)
    : client_(api_keys)  // copy — the map is tiny, we still need it below
{
    // Default memory directory is cwd-scoped ($PWD/.arbiter/memory)
    memory_dir_ = (fs::current_path() / ".arbiter" / "memory").string();

    auto master = master_constitution();
    if (auto override_id = load_master_model_override(); !override_id.empty()) {
        master.model = override_id;
    } else if (auto fallback = pick_master_model_default(api_keys); !fallback.empty()) {
        master.model = fallback;
    }
    index_master_ = std::make_unique<Agent>("index", master, client_);
}

Agent& Orchestrator::create_agent(const std::string& id, Constitution config) {
    if (id.empty())
        throw std::runtime_error("Agent ID must not be empty");
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            throw std::runtime_error(
                "Agent ID contains invalid character: '" +
                std::string(1, c) + "' — use [a-zA-Z0-9_-]");
    }
    std::lock_guard<std::mutex> lock(agents_mutex_);
    if (agents_.count(id)) {
        throw std::runtime_error("Agent already exists: " + id);
    }
    auto agent = std::make_unique<Agent>(id, std::move(config), client_);
    if (compact_cb_) agent->set_compact_callback(compact_cb_);
    auto& ref = *agent;
    agents_[id] = std::move(agent);
    return ref;
}

Agent& Orchestrator::get_agent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) throw std::runtime_error("No agent: " + id);
    return *it->second;
}

bool Orchestrator::has_agent(const std::string& id) const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    return agents_.count(id) > 0;
}

void Orchestrator::remove_agent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_.erase(id);
}

std::vector<std::string> Orchestrator::list_agents() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::vector<std::string> ids;
    ids.reserve(agents_.size());
    for (auto& [id, _] : agents_) ids.push_back(id);
    return ids;
}

void Orchestrator::load_agents(const std::string& dir) {
    if (!fs::exists(dir)) return;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            try {
                auto config = Constitution::from_file(entry.path().string());
                std::string id = config.name.empty()
                    ? entry.path().stem().string()
                    : config.name;
                create_agent(id, std::move(config));
            } catch (const std::exception& e) {
                // Skip malformed agent files, log to stderr
                fprintf(stderr, "WARN: skip %s: %s\n",
                    entry.path().c_str(), e.what());
            }
        }
    }
}

// Build an AgentInvoker that runs a sub-agent through the full dispatch loop.
AgentInvoker Orchestrator::make_invoker(const std::string& caller_id, int depth,
                                       std::map<std::string, std::string>* shared_cache,
                                       const std::string& original_query) {
    if (depth >= 2) {
        return [](const std::string&, const std::string&) -> std::string {
            return "ERR: delegation depth limit reached (max 2 levels)";
        };
    }
    return [this, caller_id, depth, shared_cache, original_query](
               const std::string& sub_id, const std::string& sub_msg) -> std::string {
        if (sub_id == caller_id) return "ERR: agent cannot invoke itself";
        if (sub_id == "index") return "ERR: index cannot be delegated to";
        {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            if (!agents_.count(sub_id))
                return "ERR: no agent '" + sub_id + "'";
        }

        // Inject delegation context so sub-agent knows the user's goal
        // and its position in the pipeline.
        // Pull recent memory entries scoped to this conversation so the
        // sub-agent walks in knowing what siblings have already recorded.
        // Without this hint scribe-style writers had no way to discover
        // scout-style researchers' /mem add entry output except by
        // guessing query terms — which fails silently and pushes the
        // agent into "graph is cold, fall back to trained knowledge"
        // mode even when the data is right there.  Cheap (one DB read
        // per /agent invocation) and degrades to nothing when no reader
        // is wired (CLI/REPL contexts have no tenant store).
        std::string pipeline_memory;
        if (structured_memory_reader_cb_) {
            try {
                std::string body = structured_memory_reader_cb_("pipeline-entries", "", caller_id);
                if (!body.empty() &&
                    body.compare(0, 4, "ERR:") != 0 &&
                    body.compare(0, 11, "(no entries") != 0) {
                    pipeline_memory = body;
                }
            } catch (...) { /* never let memory probe break delegation */ }
        }

        std::string enriched_msg;
        if (!original_query.empty()) {
            std::string truncated_query = original_query.substr(
                0, std::min<size_t>(200, original_query.size()));
            enriched_msg =
                "[DELEGATION CONTEXT]\n"
                "Original request: " + truncated_query + "\n"
                "Delegated by: " + caller_id + "\n"
                "Pipeline depth: " + std::to_string(depth + 1) + "/2\n";
            if (!pipeline_memory.empty()) {
                enriched_msg +=
                    "Pipeline memory (entries written by prior agents this "
                    "conversation — use /mem entry #<id> to read full content "
                    "before searching or restating from training):\n" +
                    pipeline_memory;
                if (pipeline_memory.back() != '\n') enriched_msg += '\n';
            }
            enriched_msg +=
                "[END DELEGATION CONTEXT]\n\n" + sub_msg;
        } else {
            enriched_msg = sub_msg;
        }

        // Run the full agentic dispatch loop for the sub-agent so it has
        // access to its own tools (/fetch, /exec, /write, /agent, /mem).
        // Shared cache propagates so sub-agents don't re-fetch URLs.
        auto resp = send_internal(sub_id, enriched_msg, depth + 1,
                                  shared_cache, original_query);
        return resp.ok ? resp.content : "ERR: " + resp.error;
    };
}

ParallelInvoker Orchestrator::make_parallel_invoker(const std::string& caller_id,
                                                     int depth,
                                                     const std::string& original_query) {
    if (depth >= 2) {
        return [](const std::vector<std::pair<std::string, std::string>>& kids) {
            return std::vector<std::string>(
                kids.size(), "ERR: /parallel cannot delegate past depth 2");
        };
    }

    return [this, caller_id, depth, original_query](
               const std::vector<std::pair<std::string, std::string>>& kids)
               -> std::vector<std::string> {
        // Each child runs on an *ephemeral* Agent — a fresh instance built
        // from the registered agent's Constitution.  Two children with the
        // same agent_id therefore can't race each other's history_ (each
        // has its own).  Cost callbacks, delegation framing, and tool-
        // result wrapping all see the public id, so the model is unaware
        // that there are multiple underlying instances — `/parallel` over
        // the same scout looks identical to two sequential calls but runs
        // concurrently.
        //
        // Trade-offs of this approach:
        //   • No history sharing across siblings or with the canonical
        //     agent.  Each child starts with the delegation-context
        //     prelude only; useful for independent research fan-out (the
        //     common case) but means a follow-up sequential `/agent X`
        //     after a `/parallel X X` block won't see the parallel turns
        //     in X's history.  Acceptable since /parallel is conceptually
        //     a fan-out, not a chained continuation.
        //   • The shared dedup cache is intentionally not propagated
        //     between siblings — see make_invoker's note.
        std::vector<std::thread> threads;
        std::vector<std::string> results(kids.size());
        threads.reserve(kids.size());

        for (size_t i = 0; i < kids.size(); ++i) {
            const std::string sub_id  = kids[i].first;
            const std::string sub_msg = kids[i].second;
            threads.emplace_back([this, i, sub_id, sub_msg, caller_id, depth,
                                   original_query, &results]() {
                // Basic validations — mirror make_invoker's gates.
                if (sub_id == caller_id) {
                    results[i] = "ERR: agent cannot invoke itself";
                    return;
                }
                if (sub_id == "index") {
                    results[i] = "ERR: index cannot be delegated to";
                    return;
                }
                Constitution cfg_copy;
                {
                    std::lock_guard<std::mutex> lk(agents_mutex_);
                    auto it = agents_.find(sub_id);
                    if (it == agents_.end()) {
                        results[i] = "ERR: no agent '" + sub_id + "'";
                        return;
                    }
                    // Copy the Constitution under the lock so we don't read
                    // while another thread is mutating the registry.  The
                    // returned cfg_copy is owned solely by this thread.
                    cfg_copy = it->second->config();
                }

                // Match make_invoker's delegation-context prelude so the
                // sub-agent has the same framing whether it was called
                // sequentially or via /parallel.  Pipeline memory snapshot
                // is taken on the spawning thread under no lock — the
                // structured_memory_reader_cb_ is set once at request setup
                // and never mutated, so concurrent reads are safe.
                std::string pipeline_memory;
                if (structured_memory_reader_cb_) {
                    try {
                        std::string body = structured_memory_reader_cb_("pipeline-entries", "", caller_id);
                        if (!body.empty() &&
                            body.compare(0, 4, "ERR:") != 0 &&
                            body.compare(0, 11, "(no entries") != 0) {
                            pipeline_memory = body;
                        }
                    } catch (...) { /* never break delegation on probe */ }
                }
                std::string enriched_msg;
                if (!original_query.empty()) {
                    std::string truncated_query = original_query.substr(
                        0, std::min<size_t>(200, original_query.size()));
                    enriched_msg =
                        "[DELEGATION CONTEXT]\n"
                        "Original request: " + truncated_query + "\n"
                        "Delegated by: " + caller_id + " (/parallel)\n"
                        "Pipeline depth: " + std::to_string(depth + 1) + "/2\n";
                    if (!pipeline_memory.empty()) {
                        enriched_msg +=
                            "Pipeline memory (entries written by prior agents "
                            "this conversation — use /mem entry #<id> to read "
                            "full content before searching or restating from "
                            "training):\n" +
                            pipeline_memory;
                        if (pipeline_memory.back() != '\n') enriched_msg += '\n';
                    }
                    enriched_msg +=
                        "[END DELEGATION CONTEXT]\n\n" + sub_msg;
                } else {
                    enriched_msg = sub_msg;
                }

                // Fresh ephemeral Agent for this child — independent
                // history_, independent stats_, no race with siblings or
                // the canonical agent registered in agents_.
                Agent ephemeral(sub_id, std::move(cfg_copy), client_);
                if (compact_cb_) ephemeral.set_compact_callback(compact_cb_);

                std::map<std::string, std::string> local_cache;
                std::string orig_q = original_query.empty() ? sub_msg : original_query;
                try {
                    auto resp = run_dispatch(ephemeral, sub_id, enriched_msg,
                                              depth + 1, &local_cache, orig_q);
                    results[i] = resp.ok ? resp.content : "ERR: " + resp.error;
                } catch (const std::exception& e) {
                    results[i] = std::string("ERR: ") + e.what();
                }
            });
        }

        for (auto& t : threads) t.join();
        return results;
    };
}

// parse_advisor_signal lives in src/advisor_gate.cpp so the gate's signal
// parser can be unit-tested without dragging the orchestrator's heavy
// dependency graph (Agent, ApiClient, MCP, …) into the test binary.

namespace {

// Build a one-line preview of the executor's terminating turn for the
// advisor-event payload.  Strips slash-command lines (they're already
// summarised separately) and clamps to a fixed budget so verbose logs
// stay tidy.  Any chunk produced by parse_agent_commands shouldn't reach
// here, so we strip raw `\n/cmd ` prefixes only.
std::string make_terminating_preview(const std::string& s, size_t max_chars = 120) {
    std::string out;
    out.reserve(std::min<size_t>(s.size(), max_chars));
    bool last_space = true;
    for (char c : s) {
        if (out.size() >= max_chars) break;
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!last_space) { out.push_back(' '); last_space = true; }
        } else {
            out.push_back(c);
            last_space = (c == ' ');
        }
    }
    if (out.size() == max_chars) out += "…";
    return out;
}

}  // namespace

AdvisorInvoker Orchestrator::make_advisor_invoker(const std::string& caller_id) {
    return [this, caller_id](const std::string& question) -> std::string {
        // Resolve the advisor model from the caller's constitution.
        std::string advisor_model;
        if (caller_id == "index") {
            advisor_model = index_master_->config().advisor_model;
        } else {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            auto it = agents_.find(caller_id);
            if (it == agents_.end()) return "ERR: no agent '" + caller_id + "'";
            advisor_model = it->second->config().advisor_model;
        }
        if (advisor_model.empty()) {
            return "ERR: no advisor_model configured for '" + caller_id + "'";
        }

        // One-shot, history-less call.  The advisor sees ONLY the question
        // text — no prior turn leaks in, so the executor must state the
        // decision and constraints in the question itself (matching how
        // Anthropic's beta advisor tool was designed to be used).  This
        // keeps advisor calls cheap, cache-friendly, and predictable.
        ApiRequest req;
        req.model               = advisor_model;
        req.max_tokens          = 1024;  // advisor replies are meant to be short
        req.include_temperature = false; // deprecated for claude-opus-4-7
        req.system_prompt =
            "You are an advisor consulted by another AI agent.  Answer the "
            "question directly and concisely.  Prescribe a specific option "
            "when the question calls for one; if you genuinely can't decide, "
            "state the tradeoff in one sentence and name the better-odds "
            "path.  No preamble.  No pleasantries.  No restating the "
            "question.  No offers to help further — the executor will "
            "re-engage if it needs more.";
        req.messages = {{"user", question}};

        ApiResponse resp = client_.complete(req);
        if (!resp.ok) return "ERR: " + resp.error;

        // Attribute the advisor's cost to the caller's ledger but use the
        // advisor model's pricing.  Accurate per-caller spend attribution
        // even when the advisor is a different provider.
        if (cost_cb_) cost_cb_(caller_id, advisor_model, resp);

        if (advisor_event_cb_) {
            AdvisorEvent ev;
            ev.agent_id  = caller_id;
            ev.stream_id = current_stream_id();
            ev.kind      = "consult";
            ev.detail    = question;
            advisor_event_cb_(ev);
        }

        return resp.content;
    };
}

AdvisorGateInvoker Orchestrator::make_advisor_gate_invoker(const std::string& caller_id) {
    return [this, caller_id](const AdvisorGateInput& in) -> AdvisorGateOutput {
        AdvisorGateOutput out;

        // Resolve the advisor model + optional prompt override from the
        // caller's constitution.  The structured `advisor` block is the
        // source of truth for gate behaviour; the legacy `advisor_model`
        // field is consulted only as a fallback when the structured model
        // is empty (which can happen if a caller wired the gate via
        // configuration outside the JSON parser path).
        std::string advisor_model;
        std::string prompt_override;
        if (caller_id == "index") {
            const auto& cfg = index_master_->config();
            advisor_model   = cfg.advisor.model.empty() ? cfg.advisor_model
                                                        : cfg.advisor.model;
            prompt_override = cfg.advisor.prompt;
        } else {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            auto it = agents_.find(caller_id);
            if (it == agents_.end()) {
                out.kind = AdvisorGateOutput::Kind::Halt;
                out.text = "no agent '" + caller_id + "' for gate";
                out.malformed = true;
                return out;
            }
            const auto& cfg = it->second->config();
            advisor_model   = cfg.advisor.model.empty() ? cfg.advisor_model
                                                        : cfg.advisor.model;
            prompt_override = cfg.advisor.prompt;
        }
        if (advisor_model.empty()) {
            // Defence-in-depth: callers should already have checked
            // mode == "gate", but if a misconfiguration slips through
            // we fail closed with a HALT explaining the issue.
            out.kind = AdvisorGateOutput::Kind::Halt;
            out.text = "no advisor model configured for gate on '" + caller_id + "'";
            out.malformed = true;
            return out;
        }

        // Default gate prompt.  Tenant can override via advisor.prompt.
        // The prompt explicitly enumerates the three signals and the
        // tag-based grammar — the parser is strict about tag form, so the
        // model must produce it verbatim.
        static constexpr const char* kDefaultGatePrompt =
            "You are a runtime gate evaluating whether an executor agent's "
            "terminating turn is acceptable to return to the caller.\n\n"
            "Inputs you receive (in this order):\n"
            "  - The original user task.\n"
            "  - The executor's outputs for the terminating turn (text only — "
            "no reasoning, no prior turns).\n"
            "  - A structured summary of tool calls made this turn.\n\n"
            "You will respond with EXACTLY ONE signal on its own line:\n\n"
            "  <signal>CONTINUE</signal>\n"
            "    The terminating turn satisfies the task; let the executor return.\n\n"
            "  <signal>REDIRECT</signal>\n"
            "  <guidance>...</guidance>\n"
            "    The executor is on the wrong track or stopped early.  Provide a "
            "concrete next step in <guidance>.  This will be injected as a "
            "synthetic user turn back to the executor.\n\n"
            "  <signal>HALT</signal>\n"
            "  <reason>...</reason>\n"
            "    The executor produced something the user must see before any "
            "further work — irreversible footgun about to commit, scope "
            "explosion, confidential data leak, fundamentally wrong premise. "
            "This will be surfaced to the user as an escalation.\n\n"
            "No preamble.  No markdown.  Output exactly one signal.  Default "
            "to CONTINUE when the turn is merely terse but correct.  Default "
            "to HALT when in doubt about safety; default to REDIRECT when in "
            "doubt about correctness.";

        std::ostringstream q;
        q << "[ORIGINAL TASK]\n" << in.original_task << "\n[END ORIGINAL TASK]\n\n"
          << "[EXECUTOR TERMINATING TURN]\n" << in.terminating_text
          << "\n[END EXECUTOR TERMINATING TURN]\n\n"
          << "[TOOL CALLS THIS TURN]\n"
          << (in.tool_summary.empty() ? "(none)\n" : in.tool_summary)
          << "[END TOOL CALLS]\n";

        ApiRequest req;
        req.model               = advisor_model;
        req.max_tokens          = 512;   // signals are short
        req.include_temperature = false; // reasoning models reject temperature
        req.system_prompt       = prompt_override.empty()
                                  ? std::string(kDefaultGatePrompt)
                                  : prompt_override;
        req.messages            = {{"user", q.str()}};

        ApiResponse resp = client_.complete(req);
        if (cost_cb_) cost_cb_(caller_id, advisor_model, resp);
        if (!resp.ok) {
            out.kind = AdvisorGateOutput::Kind::Halt;
            out.text = "advisor API error: " + resp.error;
            out.malformed = true;
            out.raw  = resp.error;
            return out;
        }

        return parse_advisor_signal(resp.content);
    };
}

// Build a one-line summary of the tool calls executed this turn for the
// advisor gate.  Format per call:
//
//   - <name> args=<first 80 chars> result=<first 200 chars>
//
// We deliberately don't include full tool-result bodies — those can be huge
// (web fetches, /exec stdout) and the advisor only needs the shape of what
// happened, not the raw data.  When the executor took no tool actions this
// returns an empty string and the gate prompt prints "(none)".
static std::string summarize_tool_calls(const std::vector<AgentCommand>& cmds,
                                        const std::string& tool_results_block) {
    if (cmds.empty()) return {};
    std::ostringstream out;
    for (auto& c : cmds) {
        std::string args = c.args;
        if (args.size() > 80) { args.resize(77); args += "..."; }
        // Locate the corresponding tool-result section in the dispatcher's
        // assembled block.  Tool results are framed as `[/<name>...]\n<body>
        // [END <NAME>]` by execute_agent_commands; we extract the first 200
        // chars of body.  If we can't find the marker, fall back to no
        // result snippet.
        std::string head_marker = "[/" + c.name;
        auto h = tool_results_block.find(head_marker);
        std::string snippet;
        if (h != std::string::npos) {
            // Move past the closing ']' of the header line, then start of body.
            auto body_start = tool_results_block.find('\n', h);
            if (body_start != std::string::npos) {
                ++body_start;
                snippet = tool_results_block.substr(body_start, 200);
                // Trim at first END marker so we don't bleed into the next call.
                auto end_marker = snippet.find("[END ");
                if (end_marker != std::string::npos) snippet.resize(end_marker);
                // Replace newlines with spaces so the summary stays one line per call.
                for (auto& ch : snippet) if (ch == '\n') ch = ' ';
                if (snippet.size() > 200) { snippet.resize(197); snippet += "..."; }
            }
        }
        out << "- " << c.name;
        if (!args.empty())    out << " args=" << args;
        if (!snippet.empty()) out << " result=" << snippet;
        out << "\n";
    }
    return out.str();
}

ApiResponse Orchestrator::ask_arbiter(const std::string& query) {
    return send("index", query);
}

void Orchestrator::set_progress_callback(ProgressCallback cb) {
    progress_cb_ = std::move(cb);
}

void Orchestrator::set_cost_callback(CostCallback cb) {
    cost_cb_ = std::move(cb);
}

void Orchestrator::set_agent_start_callback(AgentStartCallback cb) {
    start_cb_ = std::move(cb);
}

void Orchestrator::set_compact_callback(CompactCallback cb) {
    compact_cb_ = std::move(cb);
    if (index_master_) index_master_->set_compact_callback(compact_cb_);
    std::lock_guard<std::mutex> lock(agents_mutex_);
    for (auto& [_, a] : agents_) a->set_compact_callback(compact_cb_);
}

int Orchestrator::next_stream_id() {
    // Atomic so /parallel threads can grab fresh ids concurrently.  Starts
    // at -1 so the first fetch_add + 1 returns 0 for the top-level turn.
    return stream_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
}

int                Orchestrator::current_stream_id()    const { return tl_stream_id; }
const std::string& Orchestrator::current_stream_agent() const { return tl_stream_agent; }
int                Orchestrator::current_stream_depth() const { return tl_stream_depth; }

// Core agentic dispatch loop
ApiResponse Orchestrator::send_internal(const std::string& agent_id,
                                        const std::string& message,
                                        int depth,
                                        std::map<std::string, std::string>* shared_cache,
                                        const std::string& original_query) {
    // Resolve the registered Agent for this id, then hand off to
    // run_dispatch.  /parallel children with duplicate ids skip this
    // resolver path and call run_dispatch directly with ephemeral Agents.
    Agent* agent_ptr;
    std::string current_msg;
    std::map<std::string, std::string> local_cache;
    if (!shared_cache) shared_cache = &local_cache;
    std::string orig_q = original_query.empty() ? message : original_query;

    if (agent_id == "index") {
        agent_ptr   = index_master_.get();
        current_msg = global_status() + "\n\nQUERY: " + message;
    } else {
        agent_ptr   = &get_agent(agent_id);
        current_msg = message;
    }
    return run_dispatch(*agent_ptr, agent_id, current_msg, depth, shared_cache, orig_q);
}

// Parameterised dispatch loop — the body of send_internal extracted so
// the parallel invoker can pass an ephemeral cloned Agent.  agent_id is
// the public id used for callbacks and routing checks; `agent` is the
// concrete instance whose history_ this dispatch mutates.
ApiResponse Orchestrator::run_dispatch(Agent& agent,
                                        const std::string& agent_id,
                                        const std::string& current_msg_in,
                                        int depth,
                                        std::map<std::string, std::string>* shared_cache,
                                        const std::string& original_query) {
    Agent* agent_ptr   = &agent;
    std::string current_msg = current_msg_in;
    std::string orig_q       = original_query;

    auto invoker          = make_invoker(agent_id, depth, shared_cache, orig_q);
    auto advisor_invoker  = make_advisor_invoker(agent_id);
    auto parallel_invoker = make_parallel_invoker(agent_id, depth, orig_q);

    // Gate-mode advisor wiring.  Built lazily — if the agent's advisor
    // config is anything other than mode == "gate", the gate is never
    // invoked and we keep today's terminating semantics.  Building the
    // invoker here (rather than per-iteration) is cheap and lets the
    // closure capture this agent's caller_id once.
    const auto& gate_cfg = agent.config().advisor;
    const bool   gate_active = (gate_cfg.mode == "gate" && !gate_cfg.model.empty());
    AdvisorGateInvoker gate_invoker = gate_active
        ? make_advisor_gate_invoker(agent_id)
        : AdvisorGateInvoker{};

    // One stream_id per turn-sequence for this agent.  Every callback that
    // fires before this scope unwinds sees (stream_id, agent_id, depth)
    // via current_stream_*.
    const int sid = next_stream_id();
    StreamScope scope(sid, agent_id, depth);
    if (stream_start_cb_) stream_start_cb_(agent_id, sid, depth);

    // When a fleet-text callback is registered and this is a delegated
    // turn, route provider-token deltas through a per-turn StreamFilter
    // so the consumer sees clean narrative text tagged with this sub-
    // agent's (agent, sid).  Master-depth streaming still goes through
    // send_streaming's `cb` parameter; routing it here too would emit
    // every delta twice.
    const bool want_stream = (depth > 0) && static_cast<bool>(agent_stream_cb_);
    Config cfg;
    auto emit_cb = agent_stream_cb_;
    auto make_filter = [&]() -> std::unique_ptr<StreamFilter> {
        if (!want_stream) return nullptr;
        return std::make_unique<StreamFilter>(cfg,
            [emit_cb, agent_id, sid](const std::string& clean) {
                emit_cb(agent_id, sid, clean);
            });
    };

    ApiResponse resp;
    // Cumulative content + token counts across the tool-call loop.  The
    // sub-agent's caller (the master via /agent, or a parallel sibling
    // via /parallel) needs to see the full response, not just whatever
    // closing remark the last iteration produced.  Persisting this also
    // matters for resumed conversations — see send_streaming for the
    // longer rationale.
    std::string total_content;
    int         total_input_tok  = 0;
    int         total_output_tok = 0;
    static constexpr int kMaxTurns = 6;

    // Gate-mode bookkeeping.  `last_cmds` + `last_tool_results` carry the
    // most recent tool-call iteration's data so when the executor finally
    // produces a terminating turn (cmds.empty()), we can summarise the
    // tools used to get there for the advisor.  `redirects_used` enforces
    // the advisor.max_redirects budget.
    std::vector<AgentCommand> last_cmds;
    std::string               last_tool_results;
    int                       redirects_used = 0;
    const int                 max_redirects  = gate_cfg.max_redirects;

    for (int i = 0; i < kMaxTurns; ++i) {
        // Notify UI that a sub-agent is about to make an API call.
        if (depth > 0 && start_cb_) start_cb_(agent_id);

        if (want_stream) {
            // Fresh filter per turn — its internal line buffer + /write-
            // block state must not carry state across unrelated turns.
            auto filter = make_filter();
            auto* f = filter.get();
            resp = agent_ptr->stream(current_msg,
                [f](const std::string& chunk) { f->feed(chunk); });
            filter->flush();
        } else {
            resp = agent_ptr->send(current_msg);
        }
        if (!resp.ok) {
            if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
            // Surface what we got so far so a downstream caller can show
            // partial progress + the error.
            resp.content       = std::move(total_content) + resp.content;
            resp.input_tokens  = total_input_tok  + resp.input_tokens;
            resp.output_tokens = total_output_tok + resp.output_tokens;
            return resp;
        }

        if (depth > 0 && resp.ok) {
            // Notify the UI (progress) and record cost for sub-agent turns.
            // Top-level cost is recorded by the REPL after send() returns.
            if (progress_cb_) progress_cb_(agent_id, resp.content);
            if (cost_cb_)     cost_cb_(agent_id, agent_ptr->config().model, resp);
        }

        if (!total_content.empty() && total_content.back() != '\n') total_content += "\n";
        total_content    += resp.content;
        total_input_tok  += resp.input_tokens;
        total_output_tok += resp.output_tokens;

        auto cmds = parse_agent_commands(resp.content);
        recover_truncated_writes(agent_ptr, resp, cmds, nullptr);

        // Terminating branch.  If gate-mode is off, this is identical to
        // pre-gate behaviour: cmds.empty() means we're done.  With gate-
        // mode active, we consult the advisor before letting the executor
        // return — see the philosophy doc for why this gate lives below
        // the executor's API surface rather than as a slash command.
        if (cmds.empty()) {
            if (!gate_active) break;

            AdvisorGateOutput sig;
            bool budget_exhausted = (redirects_used >= max_redirects);
            if (budget_exhausted) {
                // Budget exhausted — synthesise a HALT so the advisor can't
                // pin the executor in an infinite redirect loop.  The user
                // sees the same escalation surface as a real HALT.
                sig.kind = AdvisorGateOutput::Kind::Halt;
                sig.text = "advisor redirect budget exhausted (max " +
                           std::to_string(max_redirects) + ")";
            } else {
                AdvisorGateInput in{
                    /* original_task    = */ orig_q,
                    /* terminating_text = */ resp.content,
                    /* tool_summary     = */ summarize_tool_calls(last_cmds, last_tool_results),
                };
                sig = gate_invoker(in);

                // Malformed-input policy: fail-closed by default (treat as
                // HALT) so a misbehaving advisor can't silently rubber-
                // stamp.  Configurable per agent via malformed_halts.
                if (sig.malformed && gate_cfg.malformed_halts &&
                    sig.kind != AdvisorGateOutput::Kind::Halt) {
                    sig.kind = AdvisorGateOutput::Kind::Halt;
                    sig.text = "advisor returned unparseable signal: " +
                               sig.raw.substr(0, 200);
                }
            }

            // Surface every gate decision to the verbose stream so an
            // operator watching --verbose / SSE can see the runtime gate
            // working in real time.  Includes a one-line preview of the
            // executor's terminating turn so context is visible without
            // the full transcript.
            if (advisor_event_cb_) {
                AdvisorEvent ev;
                ev.agent_id  = agent_id;
                ev.stream_id = sid;
                ev.preview   = make_terminating_preview(resp.content);
                ev.malformed = sig.malformed;
                if (budget_exhausted) {
                    ev.kind = "gate_budget";
                    ev.detail = sig.text;
                } else if (sig.kind == AdvisorGateOutput::Kind::Continue) {
                    ev.kind = "gate_continue";
                } else if (sig.kind == AdvisorGateOutput::Kind::Redirect) {
                    ev.kind = "gate_redirect";
                    ev.detail = sig.text;
                } else {
                    ev.kind = "gate_halt";
                    ev.detail = sig.text;
                }
                advisor_event_cb_(ev);
            }

            if (sig.kind == AdvisorGateOutput::Kind::Continue) break;

            if (sig.kind == AdvisorGateOutput::Kind::Redirect) {
                ++redirects_used;
                current_msg =
                    "[advisor redirect — synthetic user turn]\n" +
                    sig.text +
                    "\n[end advisor redirect]";
                // Re-enter the loop; the next iteration counts against
                // kMaxTurns the same way a tool-call iteration would.
                continue;
            }

            // HALT — surface to the user out-of-band and return ok=false.
            // Only the originating depth fires escalation_cb_; sub-agent
            // halts bubble up via resp.error_type and the caller's gate
            // (or the top-level REPL) decides what to do.
            if (depth == 0 && escalation_cb_)
                escalation_cb_(agent_id, sid, sig.text);
            if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
            resp.ok          = false;
            resp.error_type  = "advisor_halt";
            resp.error       = sig.text;
            resp.halt_reason = sig.text;
            resp.content      = std::move(total_content);
            resp.input_tokens = total_input_tok;
            resp.output_tokens = total_output_tok;
            return resp;
        }

        resp.had_tool_calls = true;
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_,
                                              invoker, confirm_cb_, shared_cache,
                                              advisor_invoker, tool_status_cb_,
                                              pane_spawner_cb_,
                                              write_interceptor_cb_,
                                              exec_disabled_,
                                              parallel_invoker,
                                              structured_memory_reader_cb_,
                                              structured_memory_writer_cb_,
                                              mcp_invoker_cb_,
                                              memory_scratchpad_cb_,
                                              search_invoker_cb_,
                                              artifact_writer_cb_,
                                              artifact_reader_cb_,
                                              artifact_lister_cb_,
                                              a2a_invoker_cb_,
                                              scheduler_invoker_cb_,
                                              agent_ptr->config().capabilities);
        // Stash for the gate's tool summary on the eventual terminating turn.
        last_cmds         = cmds;
        last_tool_results = current_msg;
    }

    // Cumulative content/tokens replace the last iteration's values so the
    // returned ApiResponse represents the full sub-agent turn.
    resp.content       = std::move(total_content);
    resp.input_tokens  = total_input_tok;
    resp.output_tokens = total_output_tok;
    if (stream_end_cb_) stream_end_cb_(agent_id, sid, resp.ok);
    return resp;
}

ApiResponse Orchestrator::send(const std::string& agent_id, const std::string& message) {
    return send_internal(agent_id, message, 0);
}

void Orchestrator::recover_truncated_writes(Agent* agent,
                                            ApiResponse& resp,
                                            std::vector<AgentCommand>& cmds,
                                            StreamCallback cb) {
    static constexpr int kMaxRetries = 3;

    for (int retry = 0; retry < kMaxRetries; ++retry) {
        std::string trunc_path;
        for (const auto& c : cmds) {
            if (c.name == "write" && c.truncated) {
                trunc_path = c.args;
                break;
            }
        }
        if (trunc_path.empty()) return;   // no unclosed /write — done

        if (cb) cb("\n\033[2m[resuming truncated /write " + trunc_path + "]\033[0m\n");

        // The previous assistant turn (with the partial /write body) is
        // already in agent history. We  just nudge it to emit the
        // remaining bytes plus /endwrite.
        std::string prompt =
            "Your previous response was cut off mid-file while writing to `" +
            trunc_path + "`.  The `/endwrite` sentinel was never emitted, so "
            "the file body is incomplete.\n\n"
            "Resume by emitting ONLY the remaining file content — the exact "
            "bytes that should follow where the previous response ended — "
            "then the literal line `/endwrite` on its own.  Do NOT repeat "
            "any content already written.  Do NOT re-emit `/write " + trunc_path +
            "`.  Do NOT add preamble, explanation, or commentary.  Start your "
            "response at the exact character where the previous response "
            "ended — even mid-word or mid-line — and close the block.";

        ApiResponse more = cb ? agent->stream(prompt, cb) : agent->send(prompt);
        if (!more.ok) return;

        resp.content               += more.content;
        resp.input_tokens          += more.input_tokens;
        resp.output_tokens         += more.output_tokens;
        resp.cache_read_tokens     += more.cache_read_tokens;
        resp.cache_creation_tokens += more.cache_creation_tokens;
        resp.stop_reason            = more.stop_reason;

        cmds = parse_agent_commands(resp.content);
    }
}

ApiResponse Orchestrator::send_streaming(const std::string& agent_id,
                                         const std::string& message,
                                         StreamCallback cb) {
    // Thin wrapper — delegate to the parts-aware overload with a single
    // text part.  Keeps every call site that builds a plain-text user
    // message untouched while letting vision callers reach the same
    // dispatch loop with image parts attached.
    std::vector<ContentPart> parts;
    ContentPart p;
    p.kind = ContentPart::TEXT;
    p.text = message;
    parts.push_back(std::move(p));
    return send_streaming(agent_id, std::move(parts), std::move(cb));
}

ApiResponse Orchestrator::send_streaming(const std::string& agent_id,
                                         std::vector<ContentPart> parts,
                                         StreamCallback cb) {
    Agent* agent_ptr;
    std::vector<ContentPart> current_parts;

    // Flatten the inbound parts to a single text string for sites that need
    // the user's request as a string — invoker construction (sub-agent
    // delegation seed), parallel-invoker context, advisor original_task.
    // Image parts contribute nothing to this text view; the model still sees
    // them via the parts vector on the wire.
    std::string message;
    for (auto& p : parts) {
        if (p.kind == ContentPart::TEXT) {
            if (!message.empty() && message.back() != '\n') message += '\n';
            message += p.text;
        }
    }

    if (agent_id == "index") {
        agent_ptr = index_master_.get();
        // Master prepends global_status() + "QUERY: " as a leading text
        // part; remaining parts (text + any image inputs) follow.
        ContentPart prefix;
        prefix.kind = ContentPart::TEXT;
        prefix.text = global_status() + "\n\nQUERY: ";
        current_parts.push_back(std::move(prefix));
        for (auto& p : parts) current_parts.push_back(std::move(p));
    } else {
        agent_ptr = &get_agent(agent_id);
        current_parts = std::move(parts);
    }

    // Helper: reset current_parts to a single text part.  Used for re-entry
    // turns whose content is pure text (advisor redirect synthesis, plain
    // tool results without image bytes).  Image-bearing tool results
    // populate current_parts directly via execute_agent_commands.
    auto set_current_text = [&](std::string text) {
        current_parts.clear();
        ContentPart p;
        p.kind = ContentPart::TEXT;
        p.text = std::move(text);
        current_parts.push_back(std::move(p));
    };

    // Top-level turn gets stream_id 0 (or the next available, if the caller
    // has already minted some via a prior call).  Fleet consumers watch
    // stream_start/end to open and close UI slots.
    const int sid = next_stream_id();
    StreamScope scope(sid, agent_id, 0);
    if (stream_start_cb_) stream_start_cb_(agent_id, sid, 0);

    // ── Master text gating ────────────────────────────────────────────
    // The orchestrator's freeform prose during an iteration that ALSO
    // emits delegation calls is "premature synthesis" — the model
    // narrates a plan and partial answer before sub-agents have produced
    // anything.  Buffer per-iteration text and decide what to do with
    // it after we've parsed the iteration's commands:
    //
    //   • iteration includes /agent or /parallel ⇒ replace buffered
    //     prose with a one-line "→ delegating: ..." status update.
    //   • iteration has no delegations ⇒ flush the buffer as the
    //     master's contribution (final synthesis or post-tool-result
    //     framing).
    //
    // Sub-agents are unaffected — their text streams live through
    // agent_stream_cb_ in send_internal.  This gate only sits between
    // the top-level master and the user-facing cb.
    std::string iter_buffer;
    auto gated_cb = [&iter_buffer](const std::string& chunk) {
        iter_buffer += chunk;
    };

    auto summarise_delegations =
        [](const std::vector<AgentCommand>& cmds) -> std::string {
        std::vector<std::string> lines;
        for (auto& c : cmds) {
            if (c.name == "agent") {
                std::string preview = c.args;
                if (preview.size() > 100) {
                    preview.resize(97);
                    preview += "...";
                }
                lines.push_back("/agent " + preview);
            } else if (c.name == "parallel") {
                int children = 0;
                for (size_t i = 0; i + 7 <= c.content.size(); ) {
                    if (c.content.compare(i, 7, "/agent ") == 0) {
                        ++children;
                        i = c.content.find('\n', i);
                        if (i == std::string::npos) break;
                        ++i;
                    } else {
                        i = c.content.find('\n', i);
                        if (i == std::string::npos) break;
                        ++i;
                    }
                }
                lines.push_back("/parallel (" + std::to_string(children) + " children)");
            }
        }
        if (lines.empty()) return std::string{};
        std::ostringstream out;
        out << "→ delegating: ";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) out << "; ";
            out << lines[i];
        }
        out << "\n";
        return out.str();
    };

    auto end_iteration = [&](const std::vector<AgentCommand>& cmds) {
        std::string status = summarise_delegations(cmds);
        if (!status.empty()) {
            // Delegation iteration — discard prose, emit status line.
            iter_buffer.clear();
            if (cb) cb(status);
        } else {
            // No delegation — flush buffered prose to the user.  Called
            // exactly once per turn for the final iteration; intermediate
            // tool-result-only iterations are rare but handled the same
            // way (their prose is the model's continuation framing).
            if (!iter_buffer.empty() && cb) cb(iter_buffer);
            iter_buffer.clear();
        }
    };

    // First turn: stream into the gate (not directly to cb).
    ApiResponse resp = agent_ptr->stream(std::move(current_parts), gated_cb);
    if (!resp.ok) {
        // On failure, flush whatever the model produced so the user sees
        // the partial — the gate's contract is to never silently swallow
        // an error response.
        if (!iter_buffer.empty() && cb) cb(iter_buffer);
        iter_buffer.clear();
        if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
        return resp;
    }
    // Bill the master's turn the same way send_internal bills delegated
    // turns.  Without this the API tenant is undercharged by the master's
    // share of provider cost; the REPL had its own post-call accounting
    // but SSE clients rely on cost_cb_ firing for every turn at every depth.
    if (cost_cb_) cost_cb_(agent_id, agent_ptr->config().model, resp);

    // Carry the cumulative response across tool-call re-entry iterations.
    // Each `agent_ptr->stream()` call returns just that iteration's text,
    // but callers (notably the API server's conversation persistence)
    // need the full assistant turn — otherwise multi-iteration turns get
    // truncated to the closing remark on save, and the next message loses
    // the prior research entirely.  Tokens get accumulated for the same
    // reason: per-iteration `resp.input_tokens`/`output_tokens` would
    // otherwise undercount the persisted turn relative to its real cost.
    std::string total_content   = resp.content;
    int         total_input_tok = resp.input_tokens;
    int         total_output_tok = resp.output_tokens;

    auto cmds = parse_agent_commands(resp.content);
    recover_truncated_writes(agent_ptr, resp, cmds, gated_cb);

    // Apply the gate decision for iteration 0 (delegation status / prose flush).
    end_iteration(cmds);

    std::map<std::string, std::string> shared_cache;
    auto invoker          = make_invoker(agent_id, 0, &shared_cache, message);
    auto advisor_invoker  = make_advisor_invoker(agent_id);
    auto parallel_invoker = make_parallel_invoker(agent_id, 0, message);

    // Gate-mode wiring (master / top-level).  Same construction as
    // run_dispatch — see the longer comment there for the reasoning.
    const auto& gate_cfg = agent_ptr->config().advisor;
    const bool   gate_active = (gate_cfg.mode == "gate" && !gate_cfg.model.empty());
    AdvisorGateInvoker gate_invoker = gate_active
        ? make_advisor_gate_invoker(agent_id)
        : AdvisorGateInvoker{};

    // Bookkeeping for the gate's terminating-turn check.  `last_cmds` +
    // `last_tool_results` carry the most recent iteration's tool-call data
    // forward so the advisor sees what the executor actually did to reach
    // its terminating output.
    std::vector<AgentCommand> last_cmds;
    std::string               last_tool_results;
    int                       redirects_used = 0;
    const int                 max_redirects  = gate_cfg.max_redirects;
    bool                      had_any_tool_calls = !cmds.empty();

    // Unified main loop — kMaxIters bounds total trips through stream(),
    // including iter 0 (already done above).  Loop body branches on
    // (cmds.empty + gate state) into one of: terminate, halt, redirect,
    // or execute-tool-results-and-stream.  Pre-gate behaviour (gate
    // inactive) is preserved exactly: cmds.empty terminates immediately.
    static constexpr int kMaxIters = 6;
    for (int i = 1; i < kMaxIters; ++i) {
        if (cmds.empty()) {
            if (!gate_active) break;

            AdvisorGateOutput sig;
            bool budget_exhausted = (redirects_used >= max_redirects);
            if (budget_exhausted) {
                sig.kind = AdvisorGateOutput::Kind::Halt;
                sig.text = "advisor redirect budget exhausted (max " +
                           std::to_string(max_redirects) + ")";
            } else {
                AdvisorGateInput in{
                    /* original_task    = */ message,
                    /* terminating_text = */ resp.content,
                    /* tool_summary     = */ summarize_tool_calls(last_cmds, last_tool_results),
                };
                sig = gate_invoker(in);
                if (sig.malformed && gate_cfg.malformed_halts &&
                    sig.kind != AdvisorGateOutput::Kind::Halt) {
                    sig.kind = AdvisorGateOutput::Kind::Halt;
                    sig.text = "advisor returned unparseable signal: " +
                               sig.raw.substr(0, 200);
                }
            }

            // Mirror gate decisions to the verbose stream — see run_dispatch
            // for the matching emit and rationale.
            if (advisor_event_cb_) {
                AdvisorEvent ev;
                ev.agent_id  = agent_id;
                ev.stream_id = sid;
                ev.preview   = make_terminating_preview(resp.content);
                ev.malformed = sig.malformed;
                if (budget_exhausted) {
                    ev.kind = "gate_budget";
                    ev.detail = sig.text;
                } else if (sig.kind == AdvisorGateOutput::Kind::Continue) {
                    ev.kind = "gate_continue";
                } else if (sig.kind == AdvisorGateOutput::Kind::Redirect) {
                    ev.kind = "gate_redirect";
                    ev.detail = sig.text;
                } else {
                    ev.kind = "gate_halt";
                    ev.detail = sig.text;
                }
                advisor_event_cb_(ev);
            }

            if (sig.kind == AdvisorGateOutput::Kind::Continue) break;

            if (sig.kind == AdvisorGateOutput::Kind::Halt) {
                if (!iter_buffer.empty() && cb) cb(iter_buffer);
                iter_buffer.clear();
                if (escalation_cb_) escalation_cb_(agent_id, sid, sig.text);
                if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
                resp.ok           = false;
                resp.error_type   = "advisor_halt";
                resp.error        = sig.text;
                resp.halt_reason  = sig.text;
                resp.content      = std::move(total_content);
                resp.input_tokens = total_input_tok;
                resp.output_tokens = total_output_tok;
                resp.had_tool_calls = had_any_tool_calls;
                return resp;
            }

            // REDIRECT — synthesise a user turn and stream the redirect.
            ++redirects_used;
            set_current_text(
                "[advisor redirect — synthetic user turn]\n" +
                sig.text +
                "\n[end advisor redirect]");
            if (cb) cb("\n");
        } else {
            // Tool-call iteration — assemble tool results and re-enter.
            if (cb) cb("\n");
            // Image-bearing tool results (/fetch on image/*, /read on image
            // artifacts) land in `image_parts` while the textual envelope
            // returns through the result string.  We then build the next
            // user-message as [text envelope, ...images], so the model sees
            // both the structured `[/fetch ...] [END FETCH]` framing and the
            // image content in the same turn.
            std::vector<ContentPart> image_parts;
            std::string tool_envelope = execute_agent_commands(cmds, agent_id, memory_dir_,
                                                  invoker, confirm_cb_, &shared_cache,
                                                  advisor_invoker, tool_status_cb_,
                                                  pane_spawner_cb_,
                                                  write_interceptor_cb_,
                                                  exec_disabled_,
                                                  parallel_invoker,
                                                  structured_memory_reader_cb_,
                                                  structured_memory_writer_cb_,
                                                  mcp_invoker_cb_,
                                                  memory_scratchpad_cb_,
                                                  search_invoker_cb_,
                                                  artifact_writer_cb_,
                                                  artifact_reader_cb_,
                                                  artifact_lister_cb_,
                                                  a2a_invoker_cb_,
                                                  scheduler_invoker_cb_,
                                                  agent_ptr->config().capabilities,
                                                  &image_parts);
            last_cmds         = cmds;
            last_tool_results = tool_envelope;
            if (image_parts.empty()) {
                set_current_text(std::move(tool_envelope));
            } else {
                current_parts.clear();
                ContentPart head;
                head.kind = ContentPart::TEXT;
                head.text = std::move(tool_envelope);
                current_parts.push_back(std::move(head));
                for (auto& img : image_parts)
                    current_parts.push_back(std::move(img));
            }
        }

        resp = agent_ptr->stream(std::move(current_parts), gated_cb);
        if (!resp.ok) {
            if (!iter_buffer.empty() && cb) cb(iter_buffer);
            iter_buffer.clear();
            if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
            resp.content        = std::move(total_content) + resp.content;
            resp.input_tokens   = total_input_tok  + resp.input_tokens;
            resp.output_tokens  = total_output_tok + resp.output_tokens;
            resp.had_tool_calls = had_any_tool_calls;
            return resp;
        }
        if (cost_cb_) cost_cb_(agent_id, agent_ptr->config().model, resp);
        if (!total_content.empty() && total_content.back() != '\n') total_content += "\n";
        total_content   += resp.content;
        total_input_tok += resp.input_tokens;
        total_output_tok += resp.output_tokens;
        cmds = parse_agent_commands(resp.content);
        recover_truncated_writes(agent_ptr, resp, cmds, gated_cb);
        end_iteration(cmds);
        if (!cmds.empty()) had_any_tool_calls = true;
    }

    // Replace per-iteration content/token counts with the cumulative values
    // before returning.  `resp` already carries the latest turn's `ok`,
    // `error`, and provider metadata, which is what we want to surface.
    resp.content        = std::move(total_content);
    resp.input_tokens   = total_input_tok;
    resp.output_tokens  = total_output_tok;
    resp.had_tool_calls = had_any_tool_calls;
    if (stream_end_cb_) stream_end_cb_(agent_id, sid, resp.ok);
    return resp;
}


std::string Orchestrator::get_agent_model(const std::string& id) const {
    if (id == "index") return index_master_->config().model;
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return "";
    return it->second->config().model;
}

const Constitution& Orchestrator::get_constitution(const std::string& id) const {
    if (id == "index") return index_master_->config();
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end())
        throw std::out_of_range("unknown agent: " + id);
    return it->second->config();
}

std::vector<std::string> Orchestrator::list_agents_all() const {
    std::vector<std::string> out;
    out.push_back("index");
    std::lock_guard<std::mutex> lock(agents_mutex_);
    out.reserve(agents_.size() + 1);
    for (auto& [id, _] : agents_) out.push_back(id);
    return out;
}

void Orchestrator::set_agent_history(const std::string& id,
                                      std::vector<Message> history) {
    if (id == "index") {
        index_master_->set_history(std::move(history));
        return;
    }
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end())
        throw std::out_of_range("unknown agent: " + id);
    it->second->set_history(std::move(history));
}

static std::string short_model(const std::string& model) {
    std::string s = model;
    if (s.size() > 7 && s.substr(0, 7) == "claude-")
        s = s.substr(7);
    // Strip trailing 8-digit date suffix
    if (s.size() > 9) {
        size_t d = s.rfind('-');
        if (d != std::string::npos && s.size() - d - 1 == 8) {
            bool all_digits = true;
            for (size_t i = d + 1; i < s.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(s[i]))) { all_digits = false; break; }
            if (all_digits) s = s.substr(0, d);
        }
    }
    return s;
}

std::string Orchestrator::global_status() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::ostringstream ss;

    if (agents_.empty()) {
        ss << "AGENTS: none loaded\n";
    } else {
        ss << "AGENTS — delegate with /agent <id> <task>:\n";
        for (auto& [id, agent] : agents_) {
            const auto& cfg = agent->config();
            // One compact line per agent: id [role] model — goal
            ss << "  " << id;
            if (!cfg.role.empty())
                ss << " [" << cfg.role << "]";
            ss << " " << short_model(cfg.model);
            if (!cfg.advisor_model.empty())
                ss << "+advisor:" << short_model(cfg.advisor_model);
            if (!cfg.goal.empty())
                ss << " — " << cfg.goal;
            ss << "\n";
        }
    }

    // Remote A2A agents from the per-request manager.  Listed under a
    // distinct section so the master can see — at routing time — the
    // trust boundary between local sub-agents (share tenant memory)
    // and remote agents (don't).  Provider is set by api_server.cpp
    // when opts.a2a_agents_path resolves to a non-empty registry.
    if (remote_roster_cb_) {
        std::string remote = remote_roster_cb_();
        if (!remote.empty()) {
            ss << "\n" << remote;
            if (remote.back() != '\n') ss << "\n";
        }
    }

    return ss.str();
}

// ─── Context compaction ───────────────────────────────────────────────────────

std::string Orchestrator::compact_agent(const std::string& agent_id) {
    if (agent_id == "index") {
        return index_master_->compact();
    }
    return get_agent(agent_id).compact();
}

// ─── Plan execution ───────────────────────────────────────────────────────────

// Parse the planner's markdown format into a list of PlanPhases.
// Recognises:
//   ### Phase N: <name>
//   **Agent:** <id>
//   **Depends on:** none | 1 | 1, 2
//   **Task:** <description>  (may span multiple lines until next **Field:** or next phase)
//   **Output:** <description>
//   **Acceptance:** <criteria>
static std::vector<Orchestrator::PlanPhase> parse_plan(const std::string& text) {
    std::vector<Orchestrator::PlanPhase> phases;
    std::istringstream ss(text);
    std::string line;

    Orchestrator::PlanPhase* cur = nullptr;
    std::string active_field;   // "task" | "output" | "acceptance" | ""

    // Flush any accumulated multi-line field into the current phase
    // (nothing to flush for single-line fields, but task can span lines)

    auto strip = [](std::string s) -> std::string {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\r')) s.erase(s.begin());
        while (!s.empty() && (s.back()  == ' ' || s.back()  == '\r')) s.pop_back();
        return s;
    };

    auto parse_depends = [](const std::string& s, std::vector<int>& out) {
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) {
            // Strip non-digits (commas, "Phase", "none")
            std::string digits;
            for (char c : tok) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
            if (!digits.empty()) out.push_back(std::stoi(digits));
        }
    };

    auto field_match = [&](const std::string& ln, const char* label, std::string& out) -> bool {
        // Match "**Label:** rest"
        std::string prefix = std::string("**") + label + ":**";
        if (ln.size() <= prefix.size()) return false;
        if (ln.substr(0, prefix.size()) != prefix) return false;
        out = strip(ln.substr(prefix.size()));
        return true;
    };

    while (std::getline(ss, line)) {
        // Strip CR
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Check for phase header: ### Phase N: Name
        if (line.size() > 10 && line.substr(0, 10) == "### Phase ") {
            phases.emplace_back();
            cur = &phases.back();
            active_field = "";
            // Parse "N: Name"
            std::string rest = line.substr(10);
            size_t colon = rest.find(':');
            if (colon != std::string::npos) {
                try { cur->number = std::stoi(rest.substr(0, colon)); } catch (...) {}
                cur->name = strip(rest.substr(colon + 1));
            } else {
                try { cur->number = std::stoi(rest); } catch (...) {}
            }
            continue;
        }

        if (!cur) continue;

        // Check for **Field:** patterns
        std::string val;
        if (field_match(line, "Agent", val)) {
            cur->agent = val;
            active_field = "";
        } else if (field_match(line, "Depends on", val)) {
            parse_depends(val, cur->depends_on);
            active_field = "";
        } else if (field_match(line, "Task", val)) {
            cur->task = val;
            active_field = "task";
        } else if (field_match(line, "Output", val)) {
            cur->output_desc = val;
            active_field = "";
        } else if (field_match(line, "Acceptance", val)) {
            cur->acceptance = val;
            active_field = "";
        } else if (!active_field.empty()) {
            // Continuation line for a multi-line field
            if (active_field == "task" && !line.empty()) {
                cur->task += "\n" + line;
            }
        }
    }

    return phases;
}

Orchestrator::PlanResult Orchestrator::execute_plan(
    const std::string& plan_path,
    std::function<void(const std::string&)> progress)
{
    PlanResult result;

    // Read plan file
    std::ifstream f(plan_path);
    if (!f.is_open()) {
        result.ok = false;
        result.error = "Cannot open plan file: " + plan_path;
        return result;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string plan_text = buf.str();

    auto phases = parse_plan(plan_text);
    if (phases.empty()) {
        result.ok = false;
        result.error = "No phases found in plan: " + plan_path;
        return result;
    }

    // Index phases by number for dependency lookup
    std::unordered_map<int, const PlanPhase*> by_number;
    for (auto& p : phases) by_number[p.number] = &p;

    // Collected outputs keyed by phase number
    std::unordered_map<int, std::string> outputs;

    // Execute phases in order (they are already listed in dependency order
    // by the planner; if a dependency hasn't run yet, we halt).
    int total = static_cast<int>(phases.size());
    for (int i = 0; i < total; ++i) {
        auto& phase = phases[i];

        if (phase.agent.empty()) {
            result.ok = false;
            result.error = "Phase " + std::to_string(phase.number) +
                           " (" + phase.name + ") has no agent assignment.";
            return result;
        }

        // Validate agent exists (skip "direct" — handled inline)
        bool is_direct = (phase.agent == "direct");
        if (!is_direct && phase.agent != "index" && !has_agent(phase.agent)) {
            result.ok = false;
            result.error = "Phase " + std::to_string(phase.number) +
                           ": agent '" + phase.agent + "' not loaded.";
            return result;
        }

        // Verify all dependencies have completed
        for (int dep : phase.depends_on) {
            if (outputs.find(dep) == outputs.end()) {
                result.ok = false;
                result.error = "Phase " + std::to_string(phase.number) +
                               " depends on Phase " + std::to_string(dep) +
                               " which has not completed (check plan order).";
                return result;
            }
        }

        // Build task message — inject dependency outputs
        std::string task_msg = phase.task;
        if (!phase.depends_on.empty()) {
            std::ostringstream ctx;
            ctx << "[PRIOR PHASE OUTPUTS]\n";
            for (int dep : phase.depends_on) {
                auto it = outputs.find(dep);
                if (it == outputs.end()) continue;
                auto dep_phase = by_number.find(dep);
                std::string dep_name = (dep_phase != by_number.end())
                    ? dep_phase->second->name : std::to_string(dep);
                ctx << "Phase " << dep << " (" << dep_name << "):\n"
                    << it->second << "\n\n";
            }
            ctx << "[END PRIOR PHASE OUTPUTS]\n\n"
                << "TASK:\n" << phase.task;
            task_msg = ctx.str();
        }

        if (progress) {
            std::string notice = "[plan] phase " + std::to_string(i + 1) + "/" +
                                 std::to_string(total) + ": " + phase.agent +
                                 " — " + phase.name;
            progress(notice);
        }

        std::string output;
        if (is_direct) {
            // "direct" phases: the task is a shell command
            output = cmd_exec(task_msg);
        } else {
            auto resp = send_internal(phase.agent, task_msg, 0);
            if (!resp.ok) {
                result.ok = false;
                result.error = "Phase " + std::to_string(phase.number) +
                               " (" + phase.agent + ") failed: " + resp.error;
                return result;
            }
            output = resp.content;
        }

        outputs[phase.number] = output;
        result.phases.emplace_back(phase.number, phase.name, output);

        if (progress) {
            progress("[plan] phase " + std::to_string(phase.number) + " complete");
        }
    }

    return result;
}

// ─── Session persistence ──────────────────────────────────────────────────────

static std::shared_ptr<JsonValue> messages_to_json(const std::vector<Message>& msgs) {
    auto arr = jarr();
    for (auto& m : msgs) {
        auto obj = jobj();
        obj->as_object_mut()["role"]    = jstr(m.role);
        obj->as_object_mut()["content"] = jstr(m.content);
        arr->as_array_mut().push_back(obj);
    }
    return arr;
}

static std::vector<Message> messages_from_json(const JsonValue* arr) {
    std::vector<Message> out;
    if (!arr || !arr->is_array()) return out;
    for (auto& v : arr->as_array()) {
        if (!v) continue;
        out.push_back({v->get_string("role"), v->get_string("content")});
    }
    return out;
}

void Orchestrator::cancel() {
    // All agents and the master share the same ApiClient instance.
    // One cancel() call interrupts any in-progress streaming across the board.
    client_.cancel();
}

void Orchestrator::save_session(const std::string& path) const {
    auto root = jobj();
    auto& m = root->as_object_mut();
    m["version"] = jnum(1);

    // Index master history
    m["index"] = messages_to_json(index_master_->history());

    // All loaded agents
    auto agents_obj = jobj();
    {
        std::lock_guard<std::mutex> lk(agents_mutex_);
        for (auto& [id, agent] : agents_) {
            if (!agent->history().empty())
                agents_obj->as_object_mut()[id] = messages_to_json(agent->history());
        }
    }
    m["agents"] = agents_obj;

    std::ofstream f(path);
    if (f.is_open()) f << json_serialize(*root);
}

bool Orchestrator::load_session(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();
    if (raw.empty()) return false;

    try {
        auto root = json_parse(raw);
        bool any_restored = false;

        // Restore index master
        auto cval = root->get("index");
        auto cmsgs = messages_from_json(cval.get());
        if (!cmsgs.empty()) {
            index_master_->set_history(std::move(cmsgs));
            any_restored = true;
        }

        // Restore loaded agents
        auto aval = root->get("agents");
        if (aval && aval->is_object()) {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            for (auto& [id, vptr] : aval->as_object()) {
                auto it = agents_.find(id);
                if (it == agents_.end()) continue;
                auto msgs = messages_from_json(vptr.get());
                if (!msgs.empty()) {
                    it->second->set_history(std::move(msgs));
                    any_restored = true;
                }
            }
        }
        return any_restored;
    } catch (...) {
        return false;
    }
}

} // namespace arbiter
