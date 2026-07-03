#pragma once
// arbiter/include/tui/tui.h
//
// Terminal UI — owns a rectangular region (a "pane") of the alternate-screen
// buffer.  Today the REPL uses one TUI instance whose rect spans the full
// terminal; the multi-pane refactor adds a layout tree where each TUI draws
// into its own rect and the same code paths work unchanged.
//
// Row layout WITHIN the pane (offsets from rect_.y, top → bottom):
//   row 1              identity + status
//                      left:  agent (bold, colored) · title (dim)
//                      right: status (when active) — else stats (dim)
//   row 2              dim separator
//   rows 3..h-3        scroll region (streamed model output lives here)
//   row  h-2           mid separator above input (doubles as pre-input status)
//   rows h-2..h-k-1    readline input area (1..kMaxInputRows, grows on wrap)
//   row  h-1           dim separator above hint row
//   row  h             hint row (key / command hints)
//
// All `*_row()` accessors return absolute 1-indexed terminal rows — they fold
// in rect_.y so call sites can pass the result straight to ANSI cursor
// positioning escapes without further arithmetic.
//
// Status is on the same row as identity; when active it preempts stats on the
// right side (stats are already dim and unimportant vs a live "thinking..."
// indicator).  A one-row blank pad sits below the input so the readline
// cursor never butts up against the bottom edge of the terminal.
//
// The mid separator has a second use: while tool calls are streaming the
// ToolCallIndicator paints its animated "⠋ N tool calls…" label onto this
// row (via set_pre_input_status / clear_pre_input_status).  Keeping tool
// output on its own row frees the header status for the thinking indicator
// — previously both fought for row 1 at 80 ms, which flashed.
//
// All stdout writes are expected to happen from a single thread (the REPL's
// main thread).  set_title() is the one exception — it holds header_mu_ so
// the async title-generation thread can update the header safely.
//
// ThinkingIndicator is a thin companion: a background thread that animates a
// "thinking..." label into the status bar until stop() is called.  It always
// operates through TUI::set_status / TUI::clear_status so it obeys the same
// save/restore-cursor invariants the rest of the layout relies on.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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
    // Header is 2 rows: identity+status, then separator.
    static constexpr int kHeaderRows    = 2;
    static constexpr int kSepRows       = 1;   // mid separator above input area
    static constexpr int kMaxInputRows  = 5;
    static constexpr int kBottomPadRows = 2;   // hint separator + hint row

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

    // No-op for backward compat.  In Stage A this entered alt-screen; that
    // moved to enter_alt_screen().  Kept for callers that still expect a
    // shutdown hook, paired with leave_alt_screen at program exit.
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

    // Accent the header bottom border when this pane is the focused one in
    // a multi-pane layout.  LayoutTree flips this on the focused leaf and
    // off on all others after every focus or structural change.  In single
    // pane mode the accent is not used.
    void set_focus_accent(bool active);

    // Blank the input rows of the pane (separator above input through
    // input bottom) without touching the rest of the chrome.  Called by
    // LayoutTree when the pane loses focus so its stale prompt text
    // doesn't linger while the active pane elsewhere handles input.
    void clear_input_area();

    // Paint a dim placeholder prompt on the pane's input row.  Used for
    // non-focused panes so their bottom edge reads as "input surface,
    // currently idle" instead of looking half-drawn.  The focused pane's
    // LineEditor overwrites this stub with the live prompt + buffer on
    // its next redraw.
    void paint_idle_input_prompt();

    // One-shot welcome card on cold starts.  build_welcome_card() returns the
    // box art; draw_welcome() pushes it into scrollback and paints (legacy).
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

    void erase_chrome_row(int row);
    void erase_pane_row(int row);

public:
    void erase_pane_row_pub(int row) { erase_pane_row(row); }
};

// Background spinner that updates TUI status state (frame redrawn by UI loop).
class ThinkingIndicator {
public:
    explicit ThinkingIndicator(TUI* tui = nullptr) : tui_(tui) {}

    void start(const std::string& label = "thinking");
    void stop();

private:
    TUI*              tui_ = nullptr;
    std::string       label_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

// Background spinner + counter for tool-call bursts.  When stacking mode is
// active (Config::verbose == false), the REPL suppresses the agent's raw
// /cmd lines from the scroll region and instead surfaces an animated
// "⠋ N tool calls…" label on the mid-separator row just above the readline
// (via TUI::set_pre_input_status).  finalize() prints a single summary row
// into scrollback — ✓ if every tool call succeeded, ✗ with the fail count
// otherwise.
//
// The indicator deliberately does NOT paint the header status row — that
// slot belongs to the ThinkingIndicator, and having both spinners repaint
// the same cell at 80 ms produced visible flashing.
//
// Lifecycle: begin() starts the spinner thread (idempotent on repeat
// begin()), bump(kind, ok) records one completed call from any delegation
// depth, finalize() stops the spinner and returns the one-line summary
// string for the caller to push into scrollback.  All calls are thread-safe
// — bump() is invoked from the orchestrator's exec thread while the spinner
// thread paints the pre-input row.
class ToolCallIndicator {
public:
    explicit ToolCallIndicator(TUI* tui = nullptr) : tui_(tui) {}

    // Arm the indicator for a new turn.  No spinner paints until bump() is
    // called at least once — the status bar should stay clean when the
    // agent's response contains no tool calls at all.
    void begin();

    // Record one completed /cmd.  First call also starts the spinner thread.
    void bump(const std::string& kind, bool ok);

    // Stop the spinner, clear the status bar, and return the scrollback
    // summary line (or empty string if no tool calls occurred this turn).
    // Thread-safe: finalize() joins the spinner thread before returning.
    std::string finalize();

    int total()  const { return total_.load(); }
    int failed() const { return failed_.load(); }

private:
    void start_spinner();
    void render_status();

    TUI*              tui_ = nullptr;
    std::atomic<bool> armed_{false};
    std::atomic<bool> running_{false};
    std::atomic<int>  total_{0};
    std::atomic<int>  failed_{0};
    std::thread       thread_;
};

} // namespace arbiter
