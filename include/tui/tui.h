#pragma once
// index/include/tui/tui.h
//
// Terminal UI — owns the alternate-screen layout for the interactive REPL.
//
// Row layout (top → bottom):
//   row 1            identity + status
//                    left:  agent (bold, colored) · title (dim)
//                    right: status (when active) — else stats (dim)
//   row 2            dim separator
//   rows 3..N-3      scroll region (streamed model output lives here)
//   row  N-2         mid separator above input (doubles as pre-input status)
//   rows N-2..N-k-1  readline input area (1..kMaxInputRows, grows on wrap)
//   row  N-1         dim separator above hint row
//   row  N           hint row (key / command hints)
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

class TUI {
public:
    // Layout constants — tuning these shifts the scroll region accordingly.
    // Header is 2 rows: row 1 identity+status, row 2 separator.
    static constexpr int kIdentityRow  = 1;
    static constexpr int kHeaderSepRow = 2;
    static constexpr int kHeaderRows   = 2;
    static constexpr int kSepRows      = 1;   // mid separator above input area
    static constexpr int kMaxInputRows = 5;
    static constexpr int kBottomPadRows = 2;  // hint separator + hint row

    // Enter alternate screen, set scroll region, draw chrome.
    void init(const std::string& agent,
              const std::string& model,
              const std::string& color = "");

    // Re-read terminal dimensions and redraw chrome (called from SIGWINCH path).
    void resize();

    // Exit alternate screen and restore the user's terminal.
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
        return rows_ - kBottomPadRows - input_rows_ - kSepRows;
    }

    // Number of visible rows in the scroll region.
    int scroll_region_rows() const { return last_scroll_row() - kHeaderRows; }

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

    int cols() const { return cols_; }
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
    int  cols_ = 80, rows_ = 24;
    int  input_rows_ = 1;
    bool status_active_ = false;
    std::atomic<bool> queue_indicator_shown_{false};
    std::string current_agent_ = "index";
    std::string current_stats_;
    std::string session_title_;
    std::string current_status_;       // cached so resize() can redraw it
    std::string current_pre_input_status_;  // inlined on sep_row() when non-empty
    mutable std::mutex header_mu_;
    std::recursive_mutex tty_mu_;      // serializes concurrent stdout writes

    int sep_row()       const { return rows_ - kBottomPadRows - input_rows_; }
    int input_top_row() const { return rows_ - kBottomPadRows - input_rows_ + 1; }
    int input_row()     const { return rows_ - kBottomPadRows; }
    int hint_sep_row()  const { return rows_ - 1; }
    int pad_row()       const { return rows_; }

    void set_scroll_region();
    void erase_chrome_row(int row);
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
