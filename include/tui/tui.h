#pragma once
// index/include/tui/tui.h
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

#include "tui/scroll_buffer.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace index_ai {

// A rectangular region of the terminal owned by one TUI instance.  Coordinates
// are 0-indexed: (x, y) is the top-left corner, w / h the interior size.  In
// single-pane mode (today) the rect spans the full terminal: {0, 0, cols, rows}.
// A future multi-pane layout simply gives each pane its own rect — the TUI's
// row/col math below is already written in terms of `rect_`, so the same code
// paints into whichever rect it owns.
struct Rect {
    int x = 0;
    int y = 0;
    int w = 80;
    int h = 24;
};

class TUI {
public:
    // Chrome layout offsets WITHIN a pane (not absolute terminal rows).
    // Header is 2 rows: identity+status, then separator.
    static constexpr int kHeaderRows    = 2;
    static constexpr int kSepRows       = 1;   // mid separator above input area
    static constexpr int kMaxInputRows  = 5;
    static constexpr int kBottomPadRows = 2;   // hint separator + hint row

    // App-level setup: enter alt-screen and clear it.  Must be called exactly
    // once per process before any TUI::init (or set_rect on a freshly-built
    // TUI).  Not a TUI method because alt-screen is a physical-terminal
    // resource shared by every pane.
    static void enter_alt_screen();

    // Exit alt-screen and restore the user's terminal.  Also static — matches
    // enter_alt_screen and can be called independent of any specific TUI.
    static void leave_alt_screen();

    // Per-pane chrome paint.  Defaults the rect to a full-screen cover for
    // single-pane callers; multi-pane callers set_rect to their pane's
    // bounds afterward (or before) without re-entering alt-screen.
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

    // Redraw the header with updated agent / stats / color.
    void update(const std::string& agent,
                const std::string& model,
                const std::string& stats,
                const std::string& color = "");

    // Draw the separator row (uses save/restore, cursor unchanged).
    void draw_sep();

    // Reset the input area to 1 row, redraw separator, park the cursor ready
    // for readline.  `pending_fn`, if provided, is queried under tty_mu_ so the
    // queue count is atomic with the status-bar repaint — passing a stale int
    // races with the exec thread popping and would leave "N queued" stuck.
    void begin_input(std::function<int()> pending_fn = {});

    // Grow the input area to `needed` rows (clamped to kMaxInputRows) when
    // readline's buffer has wrapped to another visual line.
    void grow_input(int needed);

    // Default prompt string (escape-wrapped for readline's width accounting).
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

    // Full repaint of the scroll region from a ScrollBuffer.
    //   visual_offset — visual rows above the tail (0 = live view)
    //   new_count     — new visual rows accumulated while scrolled back
    // Updates the header status line with an "↑ N lines above" indicator.
    void render_scrollback(const ScrollBuffer& buf,
                           int visual_offset, int new_count);

    // Status-bar writes.  set_status repaints; clear_status blanks the row.
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

    // One-shot welcome card painted into the middle of the scroll region on
    // cold starts (no session to restore).  Box + hello text on the left,
    // 3-line ASCII sigil on the right.  Pushes the rendered card into the
    // given scroll buffer so it's part of scrollback, and paints at the top
    // of the live scroll region so it's visible before any output arrives.
    void draw_welcome(ScrollBuffer& history);

    // Blank every row in the scroll region.  Used to dismiss the welcome
    // card the moment the user sends their first message — the REPL also
    // clears the backing ScrollBuffer so the card doesn't come back on the
    // next PgUp.  Doesn't touch chrome (header/footer/input).
    void clear_scroll_region();

    int cols() const { return rect_.w; }
    int left_col() const { return rect_.x + 1; }  // 1-indexed leftmost col
    int input_top_row_pub() const { return input_top_row(); }
    int input_bottom_row_pub() const { return input_row(); }
    int input_rows() const { return input_rows_; }

    // Thread-safe: called from the async title-generation thread.
    void set_title(const std::string& title);

    // Mutex every thread must hold while writing to stdout.  The pump thread
    // (output drain), the exec thread (tui.update / tui.set_status), and the
    // main thread (echo, begin_input) all share stdout, so serialising their
    // ANSI-escape sequences here keeps cursor save/restore pairs from
    // interleaving with each other's writes.  Readline's own writes are
    // outside the mutex — they're always single characters at the current
    // cursor position, so races there are visible but recoverable.
    // Recursive because some TUI methods call each other while both want the
    // lock — render_scrollback → set_status → draw_header, for instance.
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
    std::recursive_mutex tty_mu_;      // serializes concurrent stdout writes

    // Absolute 1-indexed terminal rows for each chrome slot within rect_.
    int identity_row()   const { return rect_.y + 1; }
    int header_sep_row() const { return rect_.y + 2; }
    int sep_row()        const { return rect_.y + rect_.h - kBottomPadRows - input_rows_; }
    int input_top_row()  const { return sep_row() + 1; }
    int input_row()      const { return rect_.y + rect_.h - kBottomPadRows; }
    int hint_sep_row()   const { return rect_.y + rect_.h - 1; }
    int pad_row()        const { return rect_.y + rect_.h; }

    void paint_chrome();          // header + separators + input row + hint
    void erase_chrome_row(int row);

    // Pane-aware line clear — writes rect_.w spaces at left_col() of `row`
    // and repositions the cursor at that left edge, ready for the caller
    // to paint content.  Replaces \033[2K everywhere; that escape clears
    // the entire physical row, which in multi-pane mode wipes whatever
    // siblings painted at the same row.
    void erase_pane_row(int row);

public:
    // Exposed for LineEditor, which paints its input row at the pane's
    // left edge with the same anti-\033[2K constraint.
    void erase_pane_row_pub(int row) { erase_pane_row(row); }
private:
    void draw_header();
    void draw_header_locked();
    void draw_footer_hint();
};

// Background spinner that ticks a "thinking..." label into TUI::set_status.
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

} // namespace index_ai
