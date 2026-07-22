#pragma once
// arbiter/include/tui/tui.h
//
// Terminal UI — per-pane chrome state (layout rects, status).  OpenTUI
// renders pixels via opentui::Session; this class holds the data pane_frame
// reads each frame.
//
// Row layout WITHIN the pane (offsets from rect_.y, top → bottom):
//   scroll region     rounded output box (floating one row below the pane
//                     top, matching sidebar inset); streamed model output
//   input area        rounded box flush beneath the output box; the top
//                     border row doubles as the status line
//   bottom pad        hint row + padding when footer is shown; with
//                     layout.chrome_compact_rows (default) multi-pane /
//                     footer-off layouts reclaim those rows for scroll
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
    int  input_rows = 1;
    int  bottom_pad_rows = 3;
    bool status_active = false;
    bool focus_accent = false;
    FooterHintMode footer_hint_mode = FooterHintMode::Full;
    // True when mode is Full or Compact (hint text may still be empty when
    // show_footer is off). Used with chrome_compact_rows to reclaim pad.
    bool footer_hint_visible = true;
    std::string status;
    std::string pre_input_status;
    // Unfocused activity badge drawn on the mid-separator when set.
    std::string activity_badge;
};

class TUI {
public:
    static constexpr int kSepRows              = 0;   // output box sits flush on input box
    static constexpr int kMaxInputRows         = 7;
    static constexpr int kBottomPadRows        = 3;   // spacer + hint row + bottom pad
    static constexpr int kCompactBottomPadRows = 1;   // trailing pad when footer reclaimed

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
    std::string build_prompt() const;

    // Last usable row of the scroll region (where streamed output lands).
    int last_scroll_row() const;

    // First row of the scroll region (top of pane).
    int scroll_top_row() const;

    // Number of visible rows in the scroll region.
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
    // uses Compact (chord-only); multi-pane unfocused uses Hidden.  When
    // layout.chrome_compact_rows is true, Hidden also reclaims those rows
    // for the scroll region; otherwise the rows stay blank so the input
    // row does not shift.
    void set_footer_hint_mode(FooterHintMode mode);

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
    int  input_rows_ = 1;
    bool status_active_ = false;
    FooterHintMode footer_hint_mode_ = FooterHintMode::Full;
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
