#include "tui/sidebar.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace arbiter {

namespace {

double estimate_cost_usd(int in_tokens, int out_tokens) {
    if (in_tokens <= 0 && out_tokens <= 0) return 0.0;
    return (static_cast<double>(in_tokens) / 1'000'000.0) * 3.0
         + (static_cast<double>(out_tokens) / 1'000'000.0) * 15.0;
}

std::string format_token_count(int n) {
    if (n < 0) n = 0;
    if (n >= 1'000'000) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1)
          << (static_cast<double>(n) / 1'000'000.0) << "M";
        return o.str();
    }
    if (n >= 10'000) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1)
          << (static_cast<double>(n) / 1'000.0) << "k";
        return o.str();
    }
    if (n >= 1'000) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1)
          << (static_cast<double>(n) / 1'000.0) << "k";
        return o.str();
    }
    return std::to_string(n);
}

std::string format_cost_usd(double usd) {
    if (usd <= 0.0) return "$0.00";
    if (usd < 0.01) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(4) << "$" << usd;
        return o.str();
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << "$" << usd;
    return o.str();
}

bool is_mcp_tool(std::string_view kind) {
    return kind == "mcp";
}

bool is_task_tool(std::string_view kind) {
    return kind == "todo";
}

void push_recent(std::vector<SidebarToolEntry>& list,
                 const std::string& name,
                 bool ok,
                 int max_entries) {
    list.insert(list.begin(), SidebarToolEntry{name, ok});
    if (static_cast<int>(list.size()) > max_entries) {
        list.resize(static_cast<size_t>(max_entries));
    }
}

} // namespace

int SidebarState::width_for_terminal(int cols) {
    if (cols < 96) return 0;
    if (cols < 120) return 24;
    return 28;
}

Rect SidebarState::rect_for_terminal(int cols, int rows) {
    const int w = width_for_terminal(cols);
    if (w <= 0 || cols <= w || rows <= 0) return {};
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
    ++turn_count_;
    last_agent_ = agent_id;
    last_model_ = model;
}

void SidebarState::record_tool(const std::string& kind, bool ok) {
    if (kind.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (is_mcp_tool(kind)) {
        push_recent(mcp_, kind, ok, kMaxRecent);
    } else if (!is_task_tool(kind)) {
        push_recent(tools_, kind, ok, kMaxRecent);
    }
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
    s.tools        = tools_;
    s.mcp          = mcp_;
    s.active_tool_calls = active_tool_calls_;
    return s;
}

std::string SidebarState::header_stats_line() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (total_input_ <= 0 && total_output_ <= 0) return {};
    const double cost = estimate_cost_usd(total_input_, total_output_);
    std::ostringstream o;
    o << "\u2191" << format_token_count(total_input_)
      << " \u2193" << format_token_count(total_output_);
    if (cost > 0.0) o << " " << format_cost_usd(cost);
    return o.str();
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
    const double cost = estimate_cost_usd(s.total_input, s.total_output);
    o << "  est:    " << format_cost_usd(cost) << " (Sonnet-equiv.)\n";
    return o.str();
}

} // namespace arbiter
