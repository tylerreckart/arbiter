#pragma once
// In-memory fleet tree + TUI sidebar state for multi-agent mission control.
//
// Consumers open a row on stream_start / agent.spawned, update it from
// agent_start / tool_call / token_usage / text / reconcile.delta, and
// close it on stream_end (keep until the job clears) or agent.teardown
// (remove immediately — JIT ephemeral lifetime).
//
// The tree is a consumer of the existing SSE / orchestrator event model.
// It does not spawn agents or change orchestrate/reconcile behavior.

#include "json.h"
#include "tui/tui.h"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

// One live or recently-ended agent slot, keyed by stream_id and/or JIT
// clone_id.  Depth is 0 (master), 1 (delegated / JIT), or 2 (sub-sub).
struct FleetNode {
    int         stream_id = -1;     // -1 until stream_start (JIT spawn first)
    std::string clone_id;           // reconcile JIT clone; empty otherwise
    std::string agent;
    int         depth = 0;
    int         parent_stream_id = -1;
    bool        ephemeral = false;  // reconcile JIT worker
    bool        running = false;
    bool        ended = false;
    bool        ok = true;
    std::string active_tool;
    int         input_tokens = 0;
    int         output_tokens = 0;
    std::string request_id;
    std::string conversation_id;
    std::string pane_agent;         // host pane's current_agent at bind
    std::vector<std::string> clauses;  // JIT cover clause ids
};

struct FleetReconcileOverlay {
    bool        active = false;
    std::string mode;     // "ensure" | "observe"
    int         wave = 0;
    int         residual = 0;
    int         held = 0;
    bool        empty = true;
    std::string request_id;
    std::string phase;    // last reconcile.progress phase
};

// Flattened, depth-indented row for paint / hit-test / keyboard.
struct FleetPaintRow {
    std::string key;        // "s:<id>" or "c:<clone_id>"
    int         stream_id = -1;
    std::string clone_id;
    std::string agent;
    int         depth = 0;
    bool        ephemeral = false;
    bool        running = false;
    bool        ended = false;
    bool        ok = true;
    std::string conversation_id;
    std::string pane_agent;
    std::string line;       // single scannable display line
};

struct FleetSnapshot {
    bool visible = false;
    bool focused = false;
    int  selected = 0;
    int  scroll_offset = 0;
    std::string request_id;
    FleetReconcileOverlay overlay;
    std::vector<FleetPaintRow> rows;
    int  overlay_lines = 0;  // painted header rows above the tree
    int  total_input = 0;
    int  total_output = 0;
    bool has_live = false;
};

enum class FleetKey {
    None,
    Up,
    Down,
    Enter,     // focus/open the pane bound to the selected row
    Escape,    // leave fleet focus (no cancel)
    Cancel,    // Esc on a running row → existing pane kill-switch
    PageUp,
    PageDown,
};

// Thread-safe event → tree state machine.  Exec threads ingest; the output
// pump snapshots for paint.
class FleetTree {
public:
    void clear();
    void on_request_received(const std::string& request_id,
                             const std::string& agent);
    void on_stream_start(const std::string& agent,
                         int stream_id,
                         int depth,
                         const std::string& request_id = {},
                         const std::string& conversation_id = {},
                         const std::string& pane_agent = {});
    void on_agent_start(const std::string& agent, int stream_id, int depth);
    void on_text(int stream_id);
    void on_tool_call(int stream_id, const std::string& tool, bool started);
    void on_token_usage(int stream_id, int input_tokens, int output_tokens);
    void on_stream_end(int stream_id, bool ok);
    void on_agent_spawned(const std::string& agent,
                          const std::string& clone_id,
                          const std::vector<std::string>& clauses,
                          const std::string& request_id = {});
    void on_agent_teardown(const std::string& clone_id);
    void on_reconcile_delta(int residual, int held, bool empty, int wave,
                            const std::string& request_id = {});
    void on_reconcile_progress(const std::string& phase,
                               const std::string& detail = {},
                               const std::string& request_id = {});
    void on_done();

    // Parse one SSE frame (tests + remote TUI).  Unknown events are ignored.
    void ingest_sse(const std::string& event, const JsonValue& payload);
    void ingest_sse(const std::string& event, const std::string& data);

    void bind_host(int stream_id,
                   const std::string& conversation_id,
                   const std::string& pane_agent);

    [[nodiscard]] std::vector<FleetNode> nodes() const;
    [[nodiscard]] FleetReconcileOverlay overlay() const;
    [[nodiscard]] std::string request_id() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool has_live() const;
    [[nodiscard]] int total_input() const;
    [[nodiscard]] int total_output() const;

    // Depth-preorder rows with indent / JIT marker / tool / tokens.
    [[nodiscard]] std::vector<FleetPaintRow> paint_rows(int max_cells = 24) const;

    [[nodiscard]] std::optional<FleetNode> find_stream(int stream_id) const;
    [[nodiscard]] std::optional<FleetNode> find_clone(const std::string& clone_id) const;

private:
    FleetNode* find_stream_locked(int stream_id);
    FleetNode* find_clone_locked(const std::string& clone_id);
    FleetNode* ensure_stream_locked(int stream_id);
    int infer_parent_locked(int depth) const;
    void attach_stream_to_clone_locked(int stream_id, const std::string& agent);

    mutable std::mutex mu_;
    std::string request_id_;
    std::vector<FleetNode> nodes_;
    FleetReconcileOverlay overlay_;
};

// Right-rail fleet dashboard.  Shown when the tree has rows (or the user
// forced it on); stays visible with multiple conversation panes.  Focus
// is a sidebar mode like ^W b, not a layout leaf — conversation panes
// keep their split tree.
class FleetSidebarState {
public:
    static constexpr int kOuterGutter = 1;
    static constexpr int kMinRemaining = 96;

    static int breakpoint_width(int cols) {
        if (cols < kMinRemaining) return 0;
        if (cols < 120) return 24;
        return 28;
    }

    void toggle_visible();
    void set_visible(bool on);
    [[nodiscard]] bool user_visible() const;
    void enter_focus();
    void exit_focus();
    [[nodiscard]] bool focused() const;

    // pane_count is ignored: fleet stays up during a multi-pane job.
    [[nodiscard]] int effective_width(int cols, int leading_cols,
                                      bool has_content) const;
    [[nodiscard]] Rect rect_for_terminal(int cols, int rows, int leading_cols,
                                         bool has_content) const;

    void move_selection(int delta, int visible_rows);
    void page_selection(int direction, int visible_rows);
    void select_at_index(int index, int visible_rows);
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] int scroll_offset() const;

    FleetKey handle_key(int key_byte, char csi_final = 0,
                        const std::string& csi_params = {});

    [[nodiscard]] FleetSnapshot snapshot(const FleetTree& tree,
                                         int cols, int leading_cols) const;

private:
    void clamp_scroll_locked(int idx, int visible_rows, int row_count);

    mutable std::mutex mu_;
    bool user_visible_ = true;
    bool focused_ = false;
    // Clamped during snapshot() so paint always gets an in-range index.
    mutable int selected_ = 0;
    mutable int scroll_offset_ = 0;
    int  last_visible_rows_ = 0;
    mutable int last_row_count_ = 0;
};

// One-line tree row: depth indent, status, name, optional jit tag, tool
// or token totals.  Width-capped for a ~24–28 col rail.
std::string format_fleet_row(const FleetNode& node,
                             bool last_sibling,
                             bool parent_last,
                             int max_cells);

// Overlay summary lines (reconcile ensure).  Empty when overlay is inactive.
std::vector<std::string> format_fleet_overlay(const FleetReconcileOverlay& ov,
                                              int max_cells);

}  // namespace arbiter
