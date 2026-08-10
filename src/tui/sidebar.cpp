#include "tui/sidebar.h"

#include "model_context.h"
#include "todo_resolve.h"
#include "tui/sidebar_format.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace arbiter {

namespace {

void push_recent(std::vector<SidebarToolEntry>& list,
                 const std::string& name,
                 bool ok,
                 int max_entries) {
    list.insert(list.begin(), SidebarToolEntry{name, ok});
    if (static_cast<int>(list.size()) > max_entries)
        list.resize(static_cast<size_t>(max_entries));
}

std::string trim_ws(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

int parse_id_token(const std::string& args) {
    std::string tok = args;
    if (!tok.empty() && tok[0] == '#') tok.erase(0, 1);
    const auto colon = tok.find(':');
    if (colon != std::string::npos) tok.resize(colon);
    tok = trim_ws(tok);
    try {
        return std::stoi(tok);
    } catch (...) {
        return 0;
    }
}

void trim_todos(std::vector<SidebarTodoEntry>& todos, int max_entries) {
    // Drop canceled rows; keep completed so the sidebar can show ✓.
    todos.erase(
        std::remove_if(todos.begin(), todos.end(),
                       [](const SidebarTodoEntry& t) {
                           return t.status == "canceled";
                       }),
        todos.end());
    if (static_cast<int>(todos.size()) <= max_entries) return;
    // Prefer dropping oldest completed first, then oldest of anything.
    while (static_cast<int>(todos.size()) > max_entries) {
        auto it = std::find_if(todos.rbegin(), todos.rend(),
                               [](const SidebarTodoEntry& t) {
                                   return t.status == "completed";
                               });
        if (it != todos.rend()) {
            todos.erase(std::next(it).base());
        } else {
            todos.pop_back();
        }
    }
}

int parse_added_todo_id(const std::string& result_preview) {
    // "OK: added #12 — subject"
    const auto hash = result_preview.find('#');
    if (hash == std::string::npos) return 0;
    size_t i = hash + 1;
    if (i >= result_preview.size() || !std::isdigit(
            static_cast<unsigned char>(result_preview[i]))) {
        return 0;
    }
    int id = 0;
    while (i < result_preview.size()
           && std::isdigit(static_cast<unsigned char>(result_preview[i]))) {
        id = id * 10 + (result_preview[i] - '0');
        if (id > 1'000'000'000) return 0;
        ++i;
    }
    return id;
}

} // namespace

int SidebarState::breakpoint_width(int cols) {
    if (cols < 96) return 0;
    if (cols < 120) return 24;
    return 28;
}

void SidebarState::toggle_visible() {
    std::lock_guard<std::mutex> lk(mu_);
    user_visible_ = !user_visible_;
}

void SidebarState::mark_prompt_started() {
    std::lock_guard<std::mutex> lk(mu_);
    prompt_started_ = true;
}

bool SidebarState::session_started() const {
    std::lock_guard<std::mutex> lk(mu_);
    return prompt_started_;
}

bool SidebarState::visible() const {
    std::lock_guard<std::mutex> lk(mu_);
    return user_visible_;
}

int SidebarState::effective_width(int cols, int pane_count,
                                  int leading_cols) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!prompt_started_ || !user_visible_) return 0;
    if (pane_count > 1) return 0;
    const int available = cols - std::max(0, leading_cols);
    return breakpoint_width(available);
}

Rect SidebarState::rect_for_terminal(int cols, int rows, int pane_count,
                                     int leading_cols) const {
    const int w = effective_width(cols, pane_count, leading_cols);
    if (w <= 0 || cols <= w + kOuterGutter || rows <= 0) return kEmptyRect;
    return Rect{cols - w - kOuterGutter, 0, w, rows};
}

void SidebarState::record_turn(const std::string& agent_id,
                               const std::string& model,
                               const ApiResponse& resp) {
    std::lock_guard<std::mutex> lk(mu_);
    last_in_  = resp.input_tokens;
    last_out_ = resp.output_tokens;
    total_input_  += resp.input_tokens;
    total_output_ += resp.output_tokens;
    total_cost_usd_ += estimate_cost_usd(model, resp.input_tokens, resp.output_tokens);
    ++turn_count_;
    last_agent_ = agent_id;
    last_model_ = model;

    if (primary_model_.empty()) {
        primary_model_ = model;
    } else if (!mixed_models_ && model != primary_model_) {
        mixed_models_ = true;
    }

    const int pct = context_pct_value(resp.input_tokens, model);
    if (pct >= 0) {
        context_tokens_      = resp.input_tokens;
        context_window_      = context_window_for_model(model);
        context_pct_current_ = pct;
        if (pct > context_pct_peak_) context_pct_peak_ = pct;
    }
}

void SidebarState::apply_todo_activity(const std::string& label, bool ok,
                                       const std::string& result_preview) {
    if (!ok) return;
    const std::string rest = label.size() > 5 ? label.substr(5) : std::string{};
    std::istringstream iss(rest);
    std::string verb;
    iss >> verb;
    std::string args;
    std::getline(iss, args);
    args = trim_ws(args);

    if (verb == "add") {
        SidebarTodoEntry e;
        e.id = parse_added_todo_id(result_preview);
        if (e.id <= 0) e.id = next_local_todo_id_++;
        else if (e.id >= next_local_todo_id_) next_local_todo_id_ = e.id + 1;
        e.subject = args.empty() ? "(untitled)" : args;
        e.status = "pending";
        todos_.insert(todos_.begin(), e);
        trim_todos(todos_, kMaxRecent);
        return;
    }

    auto resolve_local = [&](const std::string& token) -> int {
        std::vector<TodoSubjectRef> refs;
        refs.reserve(todos_.size());
        for (const auto& t : todos_) {
            if (verb == "start" || verb == "done" || verb == "cancel") {
                if (t.status == "completed") continue;
            }
            refs.push_back({t.id, t.subject});
        }
        std::string err;
        return resolve_todo_subject_ref(token, refs, err);
    };

    if (verb == "start") {
        const int id = resolve_local(args);
        if (id <= 0) return;
        for (auto& t : todos_)
            if (t.id == id) t.status = "in_progress";
    } else if (verb == "done") {
        const int id = resolve_local(args);
        if (id <= 0) return;
        for (auto& t : todos_)
            if (t.id == id) t.status = "completed";
        trim_todos(todos_, kMaxRecent);
    } else if (verb == "cancel" || verb == "delete") {
        const int id = resolve_local(args);
        if (id <= 0) return;
        todos_.erase(std::remove_if(todos_.begin(), todos_.end(),
                                    [id](const SidebarTodoEntry& t) {
                                        return t.id == id;
                                    }),
                     todos_.end());
    } else if (verb == "subject" || verb == "describe") {
        const auto colon = args.find(':');
        if (colon == std::string::npos) {
            // Persisted tool_trace stores friendly labels (`todo:subject <title>`).
            if (args.empty()) return;
            int id = 0;
            auto pick = [&](auto pred) {
                for (const auto& t : todos_) {
                    if (!pred(t)) continue;
                    if (id != 0) {
                        id = -1;
                        return;
                    }
                    id = t.id;
                }
            };
            pick([](const SidebarTodoEntry& t) {
                return t.status == "in_progress";
            });
            if (id <= 0) {
                id = 0;
                pick([](const SidebarTodoEntry& t) {
                    return t.status != "completed";
                });
            }
            if (id <= 0) return;
            for (auto& t : todos_)
                if (t.id == id) t.subject = args;
            return;
        }
        const int id = resolve_local(args.substr(0, colon));
        if (id <= 0) return;
        const std::string text = trim_ws(args.substr(colon + 1));
        for (auto& t : todos_)
            if (t.id == id && !text.empty()) t.subject = text;
    }
}

void SidebarState::apply_schedule_activity(const std::string& label, bool ok) {
    if (!ok) return;
    const std::string rest = label.size() > 9 ? label.substr(9) : std::string{};
    std::istringstream iss(rest);
    std::string verb;
    iss >> verb;
    std::string args;
    std::getline(iss, args);
    args = trim_ws(args);

    if (verb == "create") {
        SidebarScheduleEntry e;
        e.id = next_local_schedule_id_++;
        const auto colon = args.find(':');
        e.phrase = colon == std::string::npos ? args : trim_ws(args.substr(0, colon));
        if (e.phrase.empty()) e.phrase = "(scheduled)";
        e.status = "active";
        schedules_.insert(schedules_.begin(), e);
        if (static_cast<int>(schedules_.size()) > kMaxRecent)
            schedules_.resize(static_cast<size_t>(kMaxRecent));
        return;
    }

    const int id = parse_id_token(args);
    if (id <= 0) return;

    if (verb == "cancel") {
        schedules_.erase(std::remove_if(schedules_.begin(), schedules_.end(),
                                        [id](const SidebarScheduleEntry& s) {
                                            return s.id == id;
                                        }),
                          schedules_.end());
    } else if (verb == "pause") {
        for (auto& s : schedules_)
            if (s.id == id) s.status = "paused";
    } else if (verb == "resume") {
        for (auto& s : schedules_)
            if (s.id == id) s.status = "active";
    }
}

std::string SidebarState::persist_todo_label(const std::string& label) const {
    if (label.rfind("todo:", 0) != 0) return label;
    const std::string rest = label.size() > 5 ? label.substr(5) : std::string{};
    std::istringstream iss(rest);
    std::string verb;
    iss >> verb;
    std::string args;
    std::getline(iss, args);
    args = trim_ws(args);

    if (verb == "describe" || verb == "subject") {
        const auto colon = args.find(':');
        if (colon != std::string::npos) {
            const std::string id_part = trim_ws(args.substr(0, colon));
            const std::string text = trim_ws(args.substr(colon + 1));
            if (!text.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                std::vector<TodoSubjectRef> refs;
                refs.reserve(todos_.size());
                for (const auto& t : todos_) refs.push_back({t.id, t.subject});
                std::string err;
                const int id = resolve_todo_subject_ref(id_part, refs, err);
                if (id > 0) {
                    for (const auto& t : todos_) {
                        if (t.id == id)
                            return "todo:" + verb + " " + t.subject + ": " + text;
                    }
                }
            }
        }
    }

    return friendly_todo_label(label);
}

std::string SidebarState::friendly_todo_label(const std::string& label) const {
    if (label.rfind("todo:", 0) != 0) return label;
    const std::string rest = label.size() > 5 ? label.substr(5) : std::string{};
    std::istringstream iss(rest);
    std::string verb;
    iss >> verb;
    std::string args;
    std::getline(iss, args);
    args = trim_ws(args);

    if (verb.empty() || verb == "list" || verb == "add") return label;

    std::lock_guard<std::mutex> lk(mu_);

    if (verb == "describe" || verb == "subject") {
        const auto colon = args.find(':');
        if (colon != std::string::npos) {
            const std::string text = trim_ws(args.substr(colon + 1));
            if (!text.empty()) return "todo:" + verb + " " + text;
            args = trim_ws(args.substr(0, colon));
        }
    }

    std::vector<TodoSubjectRef> refs;
    refs.reserve(todos_.size());
    for (const auto& t : todos_) refs.push_back({t.id, t.subject});
    std::string err;
    const int id = resolve_todo_subject_ref(args, refs, err);
    if (id > 0) {
        for (const auto& t : todos_) {
            if (t.id == id) return "todo:" + verb + " " + t.subject;
        }
    }
    // Pure numeric id with no cached row — omit the number rather than show it.
    if (parse_id_token(args) > 0) return "todo:" + verb;
    return label;
}

void SidebarState::clear_todos() {
    std::lock_guard<std::mutex> lk(mu_);
    todos_.clear();
    next_local_todo_id_ = 1;
}

void SidebarState::record_tool(const std::string& label, bool ok,
                               const std::string& result_preview) {
    if (label.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);

    if (label.rfind("todo:", 0) == 0) {
        apply_todo_activity(label, ok, result_preview);
        return;
    }
    if (label.rfind("schedule:", 0) == 0) {
        apply_schedule_activity(label, ok);
        return;
    }
    if (label.rfind("mcp:", 0) == 0) {
        push_recent(mcp_, label.substr(4), ok, kMaxRecent);
        return;
    }
    push_recent(tools_, label, ok, kMaxRecent);
}

void SidebarState::set_active_tool_calls(int count) {
    std::lock_guard<std::mutex> lk(mu_);
    active_tool_calls_ = std::max(0, count);
}

void SidebarState::set_focus_context(const std::string& agent,
                                     const std::string& model) {
    std::lock_guard<std::mutex> lk(mu_);
    focus_agent_ = agent;
    focus_model_ = model;
}

void SidebarState::set_loops(std::vector<SidebarLoopEntry> loops) {
    std::lock_guard<std::mutex> lk(mu_);
    loops_ = std::move(loops);
}

void SidebarState::set_remote_info(std::string host, std::string tenant) {
    std::lock_guard<std::mutex> lk(mu_);
    remote_host_ = std::move(host);
    remote_tenant_ = std::move(tenant);
}

SidebarSnapshot SidebarState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    SidebarSnapshot s;
    s.total_input  = total_input_;
    s.total_output = total_output_;
    s.turn_count   = turn_count_;
    s.last_in      = last_in_;
    s.last_out     = last_out_;
    s.last_agent   = last_agent_;
    s.last_model   = last_model_;
    s.focus_agent  = focus_agent_;
    s.focus_model  = focus_model_;
    s.total_cost_usd = total_cost_usd_;
    s.cost_basis = cost_basis_label(primary_model_, mixed_models_);
    s.tools        = tools_;
    s.mcp          = mcp_;
    s.todos        = todos_;
    s.schedules    = schedules_;
    s.loops        = loops_;
    s.active_tool_calls = active_tool_calls_;
    s.user_visible = user_visible_;
    s.session_started = prompt_started_;
    s.last_context_tokens = context_tokens_;
    s.context_window      = context_window_;
    s.context_pct_current = context_pct_current_;
    s.context_pct_peak    = context_pct_peak_;
    s.remote_host         = remote_host_;
    s.remote_tenant       = remote_tenant_;
    return s;
}

std::string SidebarState::tokens_report() const {
    SidebarSnapshot s = snapshot();
    std::ostringstream o;
    o << "Session tokens\n"
      << "  input:  " << s.total_input  << " (" << format_token_count(s.total_input) << ")\n"
      << "  output: " << s.total_output << " (" << format_token_count(s.total_output) << ")\n"
      << "  turns:  " << s.turn_count << "\n";
    if (!s.last_model.empty()) {
        o << "  last:   " << s.last_agent << " / " << s.last_model << "\n"
          << "          \u2191" << s.last_in << " \u2193" << s.last_out << "\n";
    }
    o << "  est:    " << format_cost_usd(s.total_cost_usd)
      << " (" << s.cost_basis << ")\n";
    if (s.context_pct_current >= 0) {
        o << "  context: " << s.context_pct_current << "%";
        if (s.context_pct_peak > s.context_pct_current)
            o << " (peak " << s.context_pct_peak << "%)";
        o << "\n";
    }
    return o.str();
}

} // namespace arbiter
