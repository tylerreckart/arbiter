#pragma once
// arbiter/include/tui/tui.h
//
// Terminal UI — per-pane chrome state (layout rects, status).  OpenTUI
// renders pixels via opentui::Session; this class holds the data pane_frame
// reads each frame.
//
// Row layout WITHIN the pane (offsets from rect_.y, top → bottom):
//   top inset         outer-top panes float the output box one row down
//                     (matches sidebar inset); panes below a split start
//                     flush so stacked gutters stay one cell.
//   scroll region     rounded output box (scroll_top_row /
//                     scroll_region_rows already include the top inset);
//                     streamed model output
//   input area        focused pane only — rounded box flush beneath the
//                     output box; the top border row doubles as the status
//                     line. Inactive panes omit readline so stacked gutters
//                     stay a single separator cell.
//   bottom pad        outer-bottom panes reserve hint row + trailing pad so
//                     column bottoms stay aligned; stacked panes above that
//                     edge use no trailing pad so the gutter is a single
//                     separator cell (same rhythm as vertical splits).
//
// All `*_row()` accessors return absolute 1-indexed terminal rows — they fold
// in rect_.y for scroll/input placement in OpenTUI draw calls.
// bottom_pad_rows() is theme-driven (see tui_bottom_pad_rows).
//
// Tool-call / thinking spinners (set_pre_input_status / set_status) paint
// inline over the input box's top border.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace arbiter {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

inline constexpr Rect kEmptyRect{0, 0, 0, 0};

enum class FooterHintMode {
    Hidden,   // blank reserved row (unfocused multi-pane)
    Compact,  // short chord-only hint (focused multi-pane)
    Full,     // single-pane full hint
};

struct TuiChromeSnapshot {
    Rect rect;
    // 0 = content-only (inactive multi-pane); >= kDefaultInputRows when focused.
    int  input_rows = 0;
    int  bottom_pad_rows = 2;
    bool status_active = false;
    bool focus_accent = false;
    FooterHintMode footer_hint_mode = FooterHintMode::Full;
    // True when mode is Full or Compact (hint text may still be empty when
    // show_footer is off).
    bool footer_hint_visible = true;
    // True when this pane's rect touches the layout's outer bottom edge.
    // Outer-bottom panes keep a shared footer pad so column bottoms align;
    // stacked panes above that edge use no trailing pad (1-cell sep gutter).
    bool outer_bottom = true;
    // True when this pane's rect touches the layout's outer top edge.
    // Only outer-top panes keep the one-row output float (sidebar rhythm);
    // panes below a horizontal split start flush.
    bool outer_top = true;
    std::string status;
    std::string pre_input_status;
    // Unfocused activity badge drawn on the mid-separator when set.
    std::string activity_badge;
};

class TUI {
public:
    static constexpr int kSepRows              = 0;   // output box sits flush on input box
    // Rounded input box on the focused pane: top border (status) + content
    // + bottom border. Inactive panes use 0 input rows (content-only).
    static constexpr int kDefaultInputRows     = 3;
    static constexpr int kMaxInputRows         = 7;
    static constexpr int kBottomPadRows        = 2;   // hint row + bottom pad
    // Stacked (non-outer-bottom) panes: no trailing pad — the 1-cell split
    // separator is the whole gutter, matching vertical splits.
    static constexpr int kCompactBottomPadRows = 0;

    // Rows reserved below the input block (theme-aware; see tui_bottom_pad_rows).
    [[nodiscard]] int bottom_pad_rows() const;

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

    void begin_input(std::function<int()> pending_fn = {});
    void grow_input(int needed);
    // Layout sets focused panes to >= kDefaultInputRows and inactive panes
    // to 0 so readline chrome is focus-only.
    void set_input_rows(int rows);
    std::string build_prompt() const;

    // Last usable row of the scroll region (where streamed output lands).
    int last_scroll_row() const;

    // First row of the output box (1-indexed). Outer-top panes float one
    // row down; mid-stack panes start flush with the pane top.
    int scroll_top_row() const;

    // Visible rows in the output box (excludes the outer-top float).
    int scroll_region_rows() const;

    void set_status(const std::string& msg);
    void clear_status();

    // Show the queue-depth pill and suppress thinking spinner overwrites.
    void show_queue_depth(int pending);

    // Pre-input status — tool-call spinner label on the mid-separator row.
    void set_pre_input_status(const std::string& msg);
    void clear_pre_input_status();

    // Clear only the "N queued" indicator without disturbing an active spinner.
    void clear_queue_indicator();

    // True while begin_input is showing the queue-depth pill ("N queued").
    [[nodiscard]] bool queue_indicator_active() const;

    // Footer hint presentation.  Single-pane uses Full; multi-pane focused
    // outer-bottom uses Compact (chord-only); other panes use Hidden.
    // Outer-bottom panes still reserve the footer pad for column alignment;
    // chrome_compact_rows only reclaims pad when show_footer is off.
    void set_footer_hint_mode(FooterHintMode mode);

    // Whether this pane sits on the layout's outer bottom edge.  LayoutTree
    // updates this after every resize / split / focus change.
    void set_outer_bottom(bool on_bottom);
    [[nodiscard]] bool outer_bottom() const;

    // Whether this pane sits on the layout's outer top edge (controls the
    // one-row output float so stacked gutters stay a single separator cell).
    void set_outer_top(bool on_top);
    [[nodiscard]] bool outer_top() const;

    // Accent split separators when this pane is focused in a multi-pane layout.
    // LayoutTree flips this on the focused leaf and off on all others after
    // every focus or structural change.  In single-pane mode it is unused.
    void set_focus_accent(bool active);

    // Short badge for unfocused panes (e.g. "●", "✓", "✗"). Cleared on focus.
    void set_activity_badge(const std::string& badge);
    void clear_activity_badge();

    int cols() const;
    int left_col() const;  // 1-indexed leftmost col
    int input_top_row_pub() const;
    int input_bottom_row_pub() const;
    int input_rows() const;

    [[nodiscard]] TuiChromeSnapshot chrome_snapshot() const;

    std::recursive_mutex& tty_mutex() { return tty_mu_; }

private:
    Rect rect_{0, 0, 80, 24};          // area of the terminal this TUI owns
    int  input_rows_ = 0;              // 0 until focused / begin_input
    bool status_active_ = false;
    FooterHintMode footer_hint_mode_ = FooterHintMode::Full;
    bool outer_bottom_ = true;         // touches layout outer bottom edge
    bool outer_top_ = true;            // touches layout outer top edge
    bool focus_accent_ = false;        // reserved for multi-pane chrome accents
    std::atomic<bool> queue_indicator_shown_{false};
    std::string current_status_;
    std::string current_pre_input_status_;
    std::string activity_badge_;
    mutable std::recursive_mutex tty_mu_;

    // Absolute 1-indexed terminal rows for each chrome slot within rect_.
    // Uses bottom_pad_rows() so compact chrome reclaims space when the
    // footer hint is hidden.  With kSepRows==0 the last scroll row is
    // immediately above the input box (boxes share an edge).
    int sep_row()        const { return rect_.y + rect_.h - bottom_pad_rows() - input_rows_ - kSepRows; }
    int input_top_row()  const { return sep_row() + 1; }
    int input_row()      const { return rect_.y + rect_.h - bottom_pad_rows(); }
    int hint_sep_row()   const { return rect_.y + rect_.h - 1; }
    int pad_row()        const { return rect_.y + rect_.h; }
};

// Background wait-state spinner on the input-box top border (animated in the
// UI loop).  Default start() rotates friendly wait phrases with the shared
// Braille loader; an explicit label pins fixed copy (cancel / fetch / …).
class ThinkingIndicator {
public:
    explicit ThinkingIndicator(TUI* tui = nullptr) : tui_(tui) {}

    void start();                                   // rotating wait phrases
    void start(const std::string& label);           // fixed label + Braille
    void stop();
    void tick();

private:
    TUI*              tui_ = nullptr;
    std::string       label_;          // empty => rotating phrases
    std::atomic<bool> active_{false};
};

// Tool-call burst counter with a mid-separator spinner (animated in the UI loop).
class ToolCallIndicator {
public:
    explicit ToolCallIndicator(TUI* tui = nullptr) : tui_(tui) {}

    void begin();
    // Pair with bump() on Finished so the sidebar "live" count tracks
    // in-flight tools rather than cumulative finished ones.
    void on_started();
    void bump(const std::string& kind, bool ok);
    std::string finalize();
    void tick();

    int total()    const { return total_.load(); }
    int failed()   const { return failed_.load(); }
    int inflight() const {
        const int n = inflight_.load();
        return n > 0 ? n : 0;
    }

private:
    void update_status();

    TUI*              tui_ = nullptr;
    std::atomic<bool> armed_{false};
    std::atomic<bool> active_{false};
    std::atomic<int>  total_{0};
    std::atomic<int>  failed_{0};
    std::atomic<int>  inflight_{0};
};

} // namespace arbiter
