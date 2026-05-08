#pragma once
// arbiter/include/repl/pane.h
//
// A Pane is a rectangular region of the terminal that hosts its own agent
// conversation view.  It owns the visual chrome (TUI), the input surface
// (LineEditor), scrollback (ScrollBuffer), pending output (OutputQueue),
// command queue, and the session state that was previously scattered across
// the REPL main loop as a bag of local variables.
//
// In the current single-pane layout there is exactly one Pane and its TUI's
// rect spans the whole terminal.  The multi-pane refactor (Stage C) will
// introduce a layout tree that gives each Pane its own Rect; the type works
// unchanged — only the ownership / enumeration of panes moves up a level.
//
// All panes share one Orchestrator so agent state (history, memory, configs)
// is global — panes are windows onto that state, not independent worlds.
// Shared cross-pane services (LoopManager, Config, Orchestrator) live next
// to the Pane at the REPL level.

#include "repl/queues.h"
#include "tui/line_editor.h"
#include "tui/scroll_buffer.h"
#include "tui/tui.h"

#include <atomic>
#include <string>
#include <thread>

namespace arbiter {

struct Pane {
    // Declaration order matters: TUI must come first because LineEditor,
    // ThinkingIndicator, and ToolCallIndicator all hold a pointer/reference
    // into it.  Default member initializers are processed in declaration
    // order, so the &tui / tui references below are well-defined.
    TUI               tui;
    ThinkingIndicator thinking{&tui};
    ToolCallIndicator tool_indicator{&tui};
    LineEditor        editor{tui};

    // Output/input pipelines.  Exec thread drains cmd_queue and pushes to
    // output_queue; pump thread drains output_queue into scrollback + screen.
    ScrollBuffer      history;
    OutputQueue       output_queue;
    CommandQueue      cmd_queue;

    // Per-pane exec thread.  Started by the REPL after the Pane is fully
    // wired (callbacks registered on its editor, etc.); blocks on
    // cmd_queue.pop() and invokes the handle function for each line.  The
    // pane owner is responsible for: (1) cmd_queue.stop() to unblock pop,
    // (2) join() before the Pane is destroyed.
    std::thread       exec_thread;

    // Per-pane session state.  current_agent is what plain text gets sent
    // to; current_model is cached here for the header display so we don't
    // hit the orchestrator every redraw.
    std::string       current_agent = "index";
    std::string       current_model;
    std::string       multiline_accum;         // backslash-continuation buffer
    int               scroll_offset       = 0; // visual rows above the tail
    int               new_while_scrolled  = 0; // accumulated while scrolled back
    bool              welcome_visible     = true;

    // ── Delegation chain ──────────────────────────────────────────────────
    // Set when this pane was spawned by another pane via /pane.  When the
    // spawned task finishes, the pane's exec thread frames its final
    // response and queues it into parent_pane->cmd_queue so the delegating
    // agent sees its sub-work's result as a fresh message, then posts a
    // pending-close request so the REPL can prompt the user and (on yes)
    // remove this pane from the layout.  parent_pane is null for
    // user-initiated panes — they don't auto-flow-back or auto-close.
    Pane*             parent_pane         = nullptr;
    std::string       spawn_message;                         // task we were handed
    std::string       last_response;                         // captured by handle()
    std::atomic<bool> spawn_flowed{false};                   // guards single-shot flow
};

} // namespace arbiter
