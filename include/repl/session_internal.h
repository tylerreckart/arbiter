#pragma once
// Shared file-local helpers for ReplSession translation units.
// Not part of the public REPL API — include only from src/repl/*.cpp.

#include "config.h"
#include "markdown.h"
#include "render_policy.h"
#include "repl/pane.h"
#include "repl/pane_history.h"
#include "repl/queues.h"
#include "tui/tui.h"

#include <string>

namespace arbiter {

struct ApiResponse;

namespace repl_detail {

struct ReplGetcState {
    Pane* pane = nullptr;
};

extern ReplGetcState g_getc_state;
extern UiContext*    g_ui_ctx;

// Set by each pane's exec thread at startup. Orchestrator callbacks read
// this thread-local to find the owning pane with zero synchronization.
extern thread_local Pane* g_active_pane;

void wire_markdown_diff_sink(MarkdownRenderer& md, OutputQueue& oq);

RenderPolicy master_stream_policy(const Config& cfg);

// Pin the advisor gate's original task across foreground turns until the
// gate approves termination; mirrors LoopManager's original_task pinning.
void update_pane_original_task(Pane& pane,
                               const std::string& user_line,
                               const ApiResponse& resp);

// Drain any pending exec output into the pane scroll view and repaint.
void getc_flush_output();

}  // namespace repl_detail

// Re-export into arbiter for the existing call sites that used the anon-ns
// names before the peel (g_active_pane, g_getc_state, …).
using repl_detail::g_getc_state;
using repl_detail::g_ui_ctx;
using repl_detail::g_active_pane;
using repl_detail::wire_markdown_diff_sink;
using repl_detail::master_stream_policy;
using repl_detail::update_pane_original_task;
using repl_detail::getc_flush_output;

}  // namespace arbiter
