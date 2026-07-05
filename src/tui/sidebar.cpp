#include "tui/sidebar.h"

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
    todos.erase(
        std::remove_if(todos.begin(), todos.end(),
                       [](const SidebarTodoEntry& t) {
                           return t.status == "completed" || t.status == "canceled";
                       }),
        todos.end());
    if (static_cast<int>(todos.size()) > max_entries)
        todos.resize(static_cast<size_t>(max_entries));
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

int SidebarState::effective_width(int cols, int pane_count) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!prompt_started_ || !user_visible_) return 0;
    if (pane_count > 1) return 0;
    return breakpoint_width(cols);
}

Rect SidebarState::rect_for_terminal(int cols, int rows, int pane_count) const {
    const int w = effective_width(cols, pane_count);
    if (w <= 0 || cols <= w || rows <= 0) return kEmptyRect;
    return Rect{cols - w, 0, w, rows};
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

void SidebarState::apply_todo_activity(const std::string& label, bool ok) {
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
        e.id = next_local_todo_id_++;
        e.subject = args.empty() ? "(untitled)" : args;
        e.status = "pending";
        todos_.insert(todos_.begin(), e);
        trim_todos(todos_, kMaxRecent);
        return;
    }

    const int id = parse_id_token(args);
    if (id <= 0) return;

    if (verb == "start") {
        for (auto& t : todos_)
            if (t.id == id) t.status = "in_progress";
    } else if (verb == "done") {
        for (auto& t : todos_)
            if (t.id == id) t.status = "completed";
        trim_todos(todos_, kMaxRecent);
    } else if (verb == "cancel" || verb == "delete") {
        todos_.erase(std::remove_if(todos_.begin(), todos_.end(),
                                    [id](const SidebarTodoEntry& t) {
                                        return t.id == id;
                                    }),
                     todos_.end());
    } else if (verb == "subject" || verb == "describe") {
        const auto colon = args.find(':');
        if (colon != std::string::npos) {
            const std::string text = trim_ws(args.substr(colon + 1));
            for (auto& t : todos_)
                if (t.id == id && !text.empty()) t.subject = text;
        }
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

void SidebarState::record_tool(const std::string& label, bool ok) {
    if (label.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);

    if (label.rfind("todo:", 0) == 0) {
        apply_todo_activity(label, ok);
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
                                     const std::string& model,
                                     const std::string& task) {
    std::lock_guard<std::mutex> lk(mu_);
    focus_agent_ = agent;
    focus_model_ = model;
    active_task_ = task;
}

void SidebarState::set_loops(std::vector<SidebarLoopEntry> loops) {
    std::lock_guard<std::mutex> lk(mu_);
    loops_ = std::move(loops);
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
    s.active_task  = active_task_;
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
