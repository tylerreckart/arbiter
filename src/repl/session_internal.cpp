#include "repl/session_internal.h"
#include "repl/pane_history.h"
#include "api_client.h"

namespace arbiter {
namespace repl_detail {

ReplGetcState g_getc_state;
UiContext* g_ui_ctx = nullptr;
thread_local Pane* g_active_pane = nullptr;

void wire_markdown_diff_sink(MarkdownRenderer& md, OutputQueue& oq) {
    md.set_diff_sink([&oq](const std::string& patch) {
        if (!patch.empty()) oq.push_diff(patch);
    });
}

RenderPolicy master_stream_policy(const Config& cfg) {
    return cfg.verbose ? kVerbose : kMasterStream;
}

void update_pane_original_task(Pane& pane,
                               const std::string& user_line,
                               const ApiResponse& resp) {
    if (resp.ok && resp.gate_approved) {
        pane.original_task.clear();
    } else if (resp.ok && pane.original_task.empty()) {
        pane.original_task = user_line;
    }
}

void getc_flush_output() {
    auto& S = g_getc_state;
    if (!S.pane) return;
    pane_history_drain_queue(*S.pane);
    if (g_ui_ctx) pane_history_render(*S.pane, *g_ui_ctx);
}

}  // namespace repl_detail
}  // namespace arbiter
