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

    std::vector<SidebarToolEntry> tools;
    std::vector<SidebarToolEntry> mcp;

    int active_tool_calls = 0;
};

// Session-scoped sidebar data: token usage, recent tool/MCP activity, and the
// focused pane's active task.  Updated from orchestrator callbacks on exec
// threads; snapshotted under a mutex for the output pump render path.
class SidebarState {
public:
    static int width_for_terminal(int cols);
    static Rect rect_for_terminal(int cols, int rows);

    void record_turn(const std::string& agent_id,
                     const std::string& model,
                     const ApiResponse& resp);
    void record_tool(const std::string& kind, bool ok);
    void set_active_tool_calls(int count);
    void set_focus_context(const std::string& agent,
                           const std::string& model,
                           const std::string& task);

    [[nodiscard]] SidebarSnapshot snapshot() const;
    [[nodiscard]] std::string header_stats_line() const;
    [[nodiscard]] std::string tokens_report() const;

private:
    static constexpr int kMaxRecent = 8;

    mutable std::mutex mu_;
    int  total_input_  = 0;
    int  total_output_ = 0;
    int  turn_count_   = 0;
    int  last_in_      = 0;
    int  last_out_     = 0;
    std::string last_agent_;
    std::string last_model_;

    std::string focus_agent_;
    std::string focus_model_;
    std::string active_task_;
    int active_tool_calls_ = 0;

    std::vector<SidebarToolEntry> tools_;
    std::vector<SidebarToolEntry> mcp_;
};

} // namespace arbiter
