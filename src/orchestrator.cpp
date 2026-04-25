// index/src/orchestrator.cpp
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

namespace index_ai {

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
        std::string enriched_msg;
        if (!original_query.empty()) {
            std::string truncated_query = original_query.substr(
                0, std::min<size_t>(200, original_query.size()));
            enriched_msg =
                "[DELEGATION CONTEXT]\n"
                "Original request: " + truncated_query + "\n"
                "Delegated by: " + caller_id + "\n"
                "Pipeline depth: " + std::to_string(depth + 1) + "/2\n"
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
        // Agents carry per-instance history; two threads entering the same
        // Agent's chat at once would corrupt the vector.  Reject the whole
        // batch so the model retries with distinct agents or splits serially.
        std::set<std::string> seen;
        for (auto& [sid, _] : kids) {
            if (!seen.insert(sid).second) {
                return std::vector<std::string>(
                    kids.size(),
                    "ERR: /parallel cannot invoke the same agent_id twice in "
                    "one block (would race its history).  Use distinct agents, "
                    "or sequence the calls with separate /agent lines outside "
                    "/parallel.");
            }
        }

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
                {
                    std::lock_guard<std::mutex> lk(agents_mutex_);
                    if (!agents_.count(sub_id)) {
                        results[i] = "ERR: no agent '" + sub_id + "'";
                        return;
                    }
                }

                // Match make_invoker's delegation-context prelude so the
                // sub-agent has the same framing whether it was called
                // sequentially or via /parallel.
                std::string enriched_msg;
                if (!original_query.empty()) {
                    std::string truncated_query = original_query.substr(
                        0, std::min<size_t>(200, original_query.size()));
                    enriched_msg =
                        "[DELEGATION CONTEXT]\n"
                        "Original request: " + truncated_query + "\n"
                        "Delegated by: " + caller_id + " (/parallel)\n"
                        "Pipeline depth: " + std::to_string(depth + 1) + "/2\n"
                        "[END DELEGATION CONTEXT]\n\n" + sub_msg;
                } else {
                    enriched_msg = sub_msg;
                }

                // Fresh dedup cache per child (nullptr → send_internal makes
                // a local one).  Siblings fetching the same URL will both
                // fetch — fine, the alternative is a data race on the map.
                try {
                    auto resp = send_internal(sub_id, enriched_msg, depth + 1,
                                              nullptr, original_query);
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

        return resp.content;
    };
}

ApiResponse Orchestrator::ask_index_ai(const std::string& query) {
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
    Agent* agent_ptr;
    std::string current_msg;

    // At depth 0, create the shared cache and extract the original query.
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

    auto invoker          = make_invoker(agent_id, depth, shared_cache, orig_q);
    auto advisor_invoker  = make_advisor_invoker(agent_id);
    auto parallel_invoker = make_parallel_invoker(agent_id, depth, orig_q);

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
    static constexpr int kMaxTurns = 6;
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
            return resp;
        }

        if (depth > 0 && resp.ok) {
            // Notify the UI (progress) and record cost for sub-agent turns.
            // Top-level cost is recorded by the REPL after send() returns.
            if (progress_cb_) progress_cb_(agent_id, resp.content);
            if (cost_cb_)     cost_cb_(agent_id, agent_ptr->config().model, resp);
        }

        auto cmds = parse_agent_commands(resp.content);
        recover_truncated_writes(agent_ptr, resp, cmds, nullptr);
        if (cmds.empty()) break;

        resp.had_tool_calls = true;
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_,
                                              invoker, confirm_cb_, shared_cache,
                                              advisor_invoker, tool_status_cb_,
                                              pane_spawner_cb_,
                                              write_interceptor_cb_,
                                              exec_disabled_,
                                              parallel_invoker);
    }

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
    Agent* agent_ptr;
    std::string current_msg;

    if (agent_id == "index") {
        agent_ptr   = index_master_.get();
        current_msg = global_status() + "\n\nQUERY: " + message;
    } else {
        agent_ptr   = &get_agent(agent_id);
        current_msg = message;
    }

    // Top-level turn gets stream_id 0 (or the next available, if the caller
    // has already minted some via a prior call).  Fleet consumers watch
    // stream_start/end to open and close UI slots.
    const int sid = next_stream_id();
    StreamScope scope(sid, agent_id, 0);
    if (stream_start_cb_) stream_start_cb_(agent_id, sid, 0);

    // First turn: stream to caller
    ApiResponse resp = agent_ptr->stream(current_msg, cb);
    if (!resp.ok) {
        if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
        return resp;
    }
    // Bill the master's turn the same way send_internal bills delegated
    // turns.  Without this the API tenant is undercharged by the master's
    // share of provider cost; the REPL had its own post-call accounting
    // but SSE clients rely on cost_cb_ firing for every turn at every depth.
    if (cost_cb_) cost_cb_(agent_id, agent_ptr->config().model, resp);

    auto cmds = parse_agent_commands(resp.content);
    recover_truncated_writes(agent_ptr, resp, cmds, cb);
    if (cmds.empty()) {
        if (stream_end_cb_) stream_end_cb_(agent_id, sid, true);
        return resp;
    }

    std::map<std::string, std::string> shared_cache;
    auto invoker          = make_invoker(agent_id, 0, &shared_cache, message);
    auto advisor_invoker  = make_advisor_invoker(agent_id);
    auto parallel_invoker = make_parallel_invoker(agent_id, 0, message);

    // Tool-call re-entry turns: stream each so the user can follow progress
    resp.had_tool_calls = true;
    static constexpr int kMaxReentryTurns = 5;
    for (int i = 0; i < kMaxReentryTurns; ++i) {
        cb("\n");
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_,
                                              invoker, confirm_cb_, &shared_cache,
                                              advisor_invoker, tool_status_cb_,
                                              pane_spawner_cb_,
                                              write_interceptor_cb_,
                                              exec_disabled_,
                                              parallel_invoker);
        resp = agent_ptr->stream(current_msg, cb);
        if (!resp.ok) {
            if (stream_end_cb_) stream_end_cb_(agent_id, sid, false);
            return resp;
        }
        if (cost_cb_) cost_cb_(agent_id, agent_ptr->config().model, resp);
        cmds = parse_agent_commands(resp.content);
        recover_truncated_writes(agent_ptr, resp, cmds, cb);
        if (cmds.empty()) break;
        resp.had_tool_calls = true;
    }

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

} // namespace index_ai
