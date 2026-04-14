// index/src/orchestrator.cpp
#include "orchestrator.h"
#include "commands.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace index_ai {

Orchestrator::Orchestrator(const std::string& api_key)
    : client_(api_key)
{
    // Default memory directory: ~/.index/memory
    const char* home = std::getenv("HOME");
    memory_dir_ = (home ? std::string(home) : std::string(".")) + "/.index/memory";

    // Create master Index agent
    auto master = master_constitution();
    index_master_ = std::make_unique<Agent>("index", master, client_);
}

Agent& Orchestrator::create_agent(const std::string& id, Constitution config) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    if (agents_.count(id)) {
        throw std::runtime_error("Agent already exists: " + id);
    }
    auto agent = std::make_unique<Agent>(id, std::move(config), client_);
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
// Sub-agents receive their own tool access (/fetch, /exec, /write, /agent).
// depth prevents runaway delegation chains (max 2 levels: index → agent → sub-agent).
AgentInvoker Orchestrator::make_invoker(const std::string& caller_id, int depth) {
    if (depth >= 2) {
        return [](const std::string&, const std::string&) -> std::string {
            return "ERR: delegation depth limit reached (max 2 levels)";
        };
    }
    return [this, caller_id, depth](const std::string& sub_id, const std::string& sub_msg) -> std::string {
        if (sub_id == caller_id) return "ERR: agent cannot invoke itself";
        if (sub_id == "index") return "ERR: index cannot be delegated to";
        {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            if (!agents_.count(sub_id))
                return "ERR: no agent '" + sub_id + "'";
        }
        // Run the full agentic dispatch loop for the sub-agent so it has
        // access to its own tools (/fetch, /exec, /write, /agent, /mem).
        auto resp = send_internal(sub_id, sub_msg, depth + 1);
        return resp.ok ? resp.content : "ERR: " + resp.error;
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

// Core agentic dispatch loop — used by both send() and sub-agent invocations.
ApiResponse Orchestrator::send_internal(const std::string& agent_id,
                                        const std::string& message,
                                        int depth) {
    Agent* agent_ptr;
    std::string current_msg;

    if (agent_id == "index") {
        agent_ptr   = index_master_.get();
        current_msg = global_status() + "\n\nQUERY: " + message;
    } else {
        agent_ptr   = &get_agent(agent_id);
        current_msg = message;
    }

    auto invoker = make_invoker(agent_id, depth);

    ApiResponse resp;
    static constexpr int kMaxTurns = 6;
    for (int i = 0; i < kMaxTurns; ++i) {
        // Notify UI that a sub-agent is about to make an API call.
        if (depth > 0 && start_cb_) start_cb_(agent_id);

        resp = agent_ptr->send(current_msg);
        if (!resp.ok) return resp;

        if (depth > 0 && resp.ok) {
            // Notify the UI (progress) and record cost for sub-agent turns.
            // Top-level cost is recorded by the REPL after send() returns.
            if (progress_cb_) progress_cb_(agent_id, resp.content);
            if (cost_cb_)     cost_cb_(agent_id, agent_ptr->config().model, resp);
        }

        auto cmds = parse_agent_commands(resp.content);
        if (cmds.empty()) break;

        resp.had_tool_calls = true;
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_, invoker);
    }

    return resp;
}

ApiResponse Orchestrator::send(const std::string& agent_id, const std::string& message) {
    return send_internal(agent_id, message, 0);
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

    // First turn: stream to caller
    ApiResponse resp = agent_ptr->stream(current_msg, cb);
    if (!resp.ok) return resp;

    auto cmds = parse_agent_commands(resp.content);
    if (cmds.empty()) return resp;

    auto invoker = make_invoker(agent_id, 0);

    // Tool-call re-entry turns: stream each so the user can follow progress
    resp.had_tool_calls = true;
    static constexpr int kMaxReentryTurns = 5;
    for (int i = 0; i < kMaxReentryTurns; ++i) {
        cb("\n");
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_, invoker);
        resp = agent_ptr->stream(current_msg, cb);
        if (!resp.ok) return resp;
        cmds = parse_agent_commands(resp.content);
        if (cmds.empty()) break;
        resp.had_tool_calls = true;
    }

    return resp;
}


std::string Orchestrator::get_agent_model(const std::string& id) const {
    if (id == "index") return index_master_->config().model;
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return "";
    return it->second->config().model;
}

// Strip "claude-" prefix and trailing date suffix (e.g. -20250514) from model IDs.
// claude-sonnet-4-20250514 → sonnet-4
// claude-haiku-4-5-20251001 → haiku-4-5
// claude-sonnet-4-6 → sonnet-4-6
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

// Return the first N words of a rule string, with "…" if truncated.
static std::string condense_rule(const std::string& rule, int max_words = 7) {
    std::istringstream iss(rule);
    std::string word, out;
    int n = 0;
    while (iss >> word && n < max_words) {
        if (n) out += ' ';
        // Strip leading punctuation artifacts like "-" or "—"
        size_t start = word.find_first_not_of("-—•*");
        if (start != std::string::npos && start > 0) word = word.substr(start);
        if (word.empty()) continue;
        out += word;
        ++n;
    }
    if (iss >> word) out += "…";
    return out;
}

std::string Orchestrator::global_status() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::ostringstream ss;

    if (agents_.empty()) {
        ss << "AVAILABLE AGENTS: none loaded\n";
    } else {
        ss << "AVAILABLE AGENTS — delegate with /agent <id> <task>:\n";
        for (auto& [id, agent] : agents_) {
            const auto& cfg  = agent->config();
            const auto& st   = agent->stats();
            const auto& hist = agent->history();

            // Line 1: id  [role]  model  brevity  [mode:X]  [advisor:Y]
            ss << "  " << id;
            if (!cfg.role.empty())
                ss << "  [" << cfg.role << "]";
            ss << "  " << short_model(cfg.model);
            if (!cfg.mode.empty())
                ss << "  mode:" << cfg.mode;
            else
                ss << "  " << brevity_to_string(cfg.brevity);
            if (!cfg.advisor_model.empty())
                ss << "  advisor:" << short_model(cfg.advisor_model);
            ss << "\n";

            // Line 2: Goal
            if (!cfg.goal.empty())
                ss << "    Goal: " << cfg.goal << "\n";

            // Line 3: Capabilities (explicit list, or implicit from rules)
            if (!cfg.capabilities.empty()) {
                ss << "    Capabilities:";
                for (auto& cap : cfg.capabilities) ss << " " << cap;
                ss << "\n";
            }

            // Line 4: Rules — condensed to first 7 words each, joined with ·
            if (!cfg.rules.empty()) {
                ss << "    Rules: ";
                bool first = true;
                for (auto& r : cfg.rules) {
                    std::string condensed = condense_rule(r);
                    if (condensed.empty()) continue;
                    if (!first) ss << " · ";
                    ss << condensed;
                    first = false;
                }
                ss << "\n";
            }

            // Line 5: context depth + request stats
            ss << "    Context: " << hist.size() << " msgs"
               << "  reqs:" << st.total_requests
               << "  in:" << st.total_input_tokens
               << "  out:" << st.total_output_tokens;
            if (!agent->context_summary().empty())
                ss << "  [compacted]";
            ss << "\n";
        }
    }

    ss << "SESSION: in:" << client_.total_input_tokens()
       << " out:" << client_.total_output_tokens() << "\n";

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
