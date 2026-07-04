#pragma once
// arbiter/include/tui/tui.h
//
// Terminal UI — per-pane chrome state (layout rects, status, title).  OpenTUI
// renders pixels via opentui::Session; this class holds the data pane_frame
// reads each frame.
//
// Row layout WITHIN the pane (offsets from rect_.y, top → bottom):
//   row 1              top padding
//   rows 2..4          identity + status block
//                      left:  agent (bold, colored) · title (dim)
//                      right: status (when active) — else stats (dim)
//   rows 5..h-5        scroll region (streamed model output lives here)
//   row  h-6           content padding / pre-input status
//   rows h-6..h-3      input area (readline block)
//   row  h-2           content padding below readline
//   row  h-1           hint row (key / command hints)
//   row  h             bottom padding
//
// All `*_row()` accessors return absolute 1-indexed terminal rows — they fold
// in rect_.y for scroll/input placement in OpenTUI draw calls.
//
// Status is on the same row as identity; when active it preempts stats on the
// right side (stats are already dim and unimportant vs a live "thinking..."
// indicator).
//
// The mid separator has a second use: while tool calls are streaming the
// ToolCallIndicator paints its animated "⠋ N tool calls…" label onto this
// row (via set_pre_input_status / clear_pre_input_status).  Keeping tool
// output on its own row frees the header status for the thinking indicator
// — previously both fought for row 1 at 80 ms, which flashed.
//
// set_title() holds header_mu_ so the async title-generation thread can update
// the header safely.  Spinner frames advance in the output pump (no thread).

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace arbiter {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 80;
    int h = 24;
};

struct TuiChromeSnapshot {
    Rect rect;
    int  input_rows = 1;
    bool status_active = false;
    bool focus_accent = false;
    bool footer_hint_visible = true;
    std::string agent;
    std::string title;
    std::string status;
    std::string stats;
    std::string pre_input_status;
};

class TUI {
public:
    // Chrome layout offsets WITHIN a pane (not absolute terminal rows).
    // Header is 4 rows: top padding, then a three-row header block.
    static constexpr int kHeaderRows    = 4;
    static constexpr int kSepRows       = 1;   // mid separator above input area
    static constexpr int kMaxInputRows  = 7;
    static constexpr int kBottomPadRows = 3;   // input spacer + hint row + bottom padding

    // Per-pane layout.  Rendering is handled by OpenTUI (see opentui::Session).
    void init(const std::string& agent,
              const std::string& model,
              const std::string& color = "");

    // Resize this pane's owned rect to a new area and repaint chrome.  Does
    // not touch alt-screen or clear the whole terminal — that's a layout-
    // level concern (the app clears the screen once before asking every
    // pane to set_rect/redraw).
    void set_rect(const Rect& r);

    // Re-read terminal dimensions and redraw chrome (called from SIGWINCH
    // path).  In single-pane mode the rect becomes {0,0,cols,rows}; in
    // multi-pane mode the layout recomputes each pane's rect and calls
    // set_rect instead — this method is a convenience for the single-pane
    // case where no layout tree exists.
    void resize();

    // No-op — terminal lifecycle is owned by opentui::Session.
    void shutdown();

    void update(const std::string& agent,
                const std::string& model,
                const std::string& stats,
                const std::string& color = "");

    void begin_input(std::function<int()> pending_fn = {});
    void grow_input(int needed);
    std::string build_prompt() const;

    // Last usable row of the scroll region (where streamed output lands).
    int last_scroll_row() const {
        return rect_.y + rect_.h - kBottomPadRows - input_rows_ - kSepRows;
    }

    // First row of the scroll region (just below the header separator).
    int scroll_top_row() const { return rect_.y + kHeaderRows + 1; }

    // Number of visible rows in the scroll region.
    int scroll_region_rows() const {
        return last_scroll_row() - scroll_top_row() + 1;
    }

    void set_status(const std::string& msg);
    void clear_status();

    // Pre-input status — a dim label inlined with the mid-separator row just
    // above the readline.  Used by ToolCallIndicator so its spinner doesn't
    // share row 1 with the header thinking indicator.  An empty label (or
    // clear) restores the plain dashed separator.  Cheap; safe to call from
    // background threads (guarded by tty_mu_).
    void set_pre_input_status(const std::string& msg);
    void clear_pre_input_status();

    // Clear only the "N queued" indicator without disturbing an active spinner.
    void clear_queue_indicator();

    // Show / hide the two-row footer hint at the bottom of the pane.  In
    // single-pane mode the hint ("esc interrupt, pgup/dn scroll, /agents,
    // /help") is useful; in multi-pane layouts it becomes clutter on every
    // pane.  LayoutTree::resize toggles this for every leaf whenever the
    // pane count crosses the 1/>1 boundary.  The rows are still reserved
    // (blanked) when hidden so the input row's absolute position doesn't
    // shift between modes.
    void set_footer_hint_visible(bool visible);

    // Accent split separators when this pane is focused in a multi-pane layout.
    // LayoutTree flips this on the focused leaf and off on all others after
    // every focus or structural change.  In single-pane mode it is unused.
    void set_focus_accent(bool active);

    // One-shot welcome card on cold starts (ANSI box art pushed to scrollback).
    [[nodiscard]] std::string build_welcome_card() const;

    int cols() const { return rect_.w; }
    int left_col() const { return rect_.x + 1; }  // 1-indexed leftmost col
    int input_top_row_pub() const { return input_top_row(); }
    int input_bottom_row_pub() const { return input_row(); }
    int input_rows() const { return input_rows_; }

    // Thread-safe: called from the async title-generation thread.
    void set_title(const std::string& title);

    [[nodiscard]] TuiChromeSnapshot chrome_snapshot() const;

    std::recursive_mutex& tty_mutex() { return tty_mu_; }

private:
    Rect rect_{0, 0, 80, 24};          // area of the terminal this TUI owns
    int  input_rows_ = 1;
    bool status_active_ = false;
    bool footer_hint_visible_ = true;  // flipped off in multi-pane layouts
    bool focus_accent_ = false;        // accent header bottom border when focused
    std::atomic<bool> queue_indicator_shown_{false};
    std::string current_agent_ = "index";
    std::string current_stats_;
    std::string session_title_;
    std::string current_status_;       // cached so resize() can redraw it
    std::string current_pre_input_status_;  // inlined on sep_row() when non-empty
    mutable std::mutex header_mu_;
    mutable std::recursive_mutex tty_mu_;

    // Absolute 1-indexed terminal rows for each chrome slot within rect_.
    int identity_row()   const { return rect_.y + 1; }
    int header_sep_row() const { return rect_.y + 2; }
    int sep_row()        const { return rect_.y + rect_.h - kBottomPadRows - input_rows_; }
    int input_top_row()  const { return sep_row() + 1; }
    int input_row()      const { return rect_.y + rect_.h - kBottomPadRows; }
    int hint_sep_row()   const { return rect_.y + rect_.h - 1; }
    int pad_row()        const { return rect_.y + rect_.h; }
};

// Background spinner that updates TUI status state (animated in the UI loop).
class ThinkingIndicator {
public:
    explicit ThinkingIndicator(TUI* tui = nullptr) : tui_(tui) {}

    void start(const std::string& label = "thinking");
    void stop();
    void tick();

private:
    TUI*              tui_ = nullptr;
    std::string       label_;
    std::atomic<bool> active_{false};
};

// Tool-call burst counter with a mid-separator spinner (animated in the UI loop).
class ToolCallIndicator {
public:
    explicit ToolCallIndicator(TUI* tui = nullptr) : tui_(tui) {}

    void begin();
    void bump(const std::string& kind, bool ok);
    std::string finalize();
    void tick();

    int total()  const { return total_.load(); }
    int failed() const { return failed_.load(); }

private:
    void update_status();

    TUI*              tui_ = nullptr;
    std::atomic<bool> armed_{false};
    std::atomic<bool> active_{false};
    std::atomic<int>  total_{0};
    std::atomic<int>  failed_{0};
};

} // namespace arbiter
