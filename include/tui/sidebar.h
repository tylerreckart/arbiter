#pragma once

#include "api_client.h"
#include "tui/tui.h"

#include <mutex>
#include <string>
#include <vector>

namespace arbiter {

struct SidebarToolEntry {
    std::string name;
    bool        ok = true;
};

struct SidebarTodoEntry {
    int         id = 0;
    std::string subject;
    std::string status;  // pending, in_progress
};

struct SidebarScheduleEntry {
    int64_t     id = 0;
    std::string phrase;
    std::string status;  // active, paused
};

struct SidebarLoopEntry {
    std::string id;
    std::string agent_id;
    std::string state;
    int         iter = 0;
};

struct SidebarSnapshot {
    int  total_input  = 0;
    int  total_output = 0;
    int  turn_count   = 0;
    int  last_in      = 0;
    int  last_out     = 0;
    std::string last_agent;
    std::string last_model;

    std::string focus_agent;
    std::string focus_model;
    std::string active_task;

    double      total_cost_usd = 0.0;
    std::string cost_basis;

    std::vector<SidebarToolEntry> tools;
    std::vector<SidebarToolEntry> mcp;

    std::vector<SidebarTodoEntry>     todos;
    std::vector<SidebarScheduleEntry> schedules;
    std::vector<SidebarLoopEntry>     loops;

    int active_tool_calls = 0;
    bool user_visible     = true;
    bool session_started  = false;

    int         last_context_tokens = 0;
    int         context_window      = 0;
    int         context_pct_current = -1;
    int         context_pct_peak      = -1;
};

// Session-scoped sidebar data: token usage, recent tool/MCP activity, and the
// focused pane's active task.  Updated from orchestrator callbacks on exec
// threads; snapshotted under a mutex for the output pump render path.
class SidebarState {
public:
    static int breakpoint_width(int cols);

    void toggle_visible();
    void mark_prompt_started();
    [[nodiscard]] bool session_started() const;
    [[nodiscard]] bool visible() const;
    // pane_count > 1 hides the sidebar (multi-pane layouts need full width).
    // leading_cols: width reserved by the history sidebar on the left.
    [[nodiscard]] int effective_width(int cols, int pane_count = 1,
                                      int leading_cols = 0) const;
    [[nodiscard]] Rect rect_for_terminal(int cols, int rows,
                                         int pane_count = 1,
                                         int leading_cols = 0) const;

    void record_turn(const std::string& agent_id,
                     const std::string& model,
                     const ApiResponse& resp);
    void record_tool(const std::string& label, bool ok);
    void set_active_tool_calls(int count);
    void set_focus_context(const std::string& agent,
                           const std::string& model,
                           const std::string& task);
    void set_loops(std::vector<SidebarLoopEntry> loops);

    [[nodiscard]] SidebarSnapshot snapshot() const;
    [[nodiscard]] std::string tokens_report() const;

private:
    static constexpr int kMaxRecent = 8;

    void apply_todo_activity(const std::string& label, bool ok);
    void apply_schedule_activity(const std::string& label, bool ok);

    mutable std::mutex mu_;
    bool user_visible_    = true;
    bool prompt_started_  = false;

    int  total_input_  = 0;
    int  total_output_ = 0;
    int  turn_count_   = 0;
    int  last_in_      = 0;
    int  last_out_     = 0;
    std::string last_agent_;
    std::string last_model_;

    double      total_cost_usd_ = 0.0;
    std::string primary_model_;
    bool        mixed_models_   = false;

    std::string focus_agent_;
    std::string focus_model_;
    std::string active_task_;
    int active_tool_calls_ = 0;

    int context_tokens_     = 0;
    int context_window_     = 0;
    int context_pct_current_ = -1;
    int context_pct_peak_    = -1;

    int next_local_todo_id_     = 1;
    int next_local_schedule_id_ = 1;

    std::vector<SidebarToolEntry> tools_;
    std::vector<SidebarToolEntry> mcp_;
    std::vector<SidebarTodoEntry> todos_;
    std::vector<SidebarScheduleEntry> schedules_;
    std::vector<SidebarLoopEntry> loops_;
};

} // namespace arbiter
