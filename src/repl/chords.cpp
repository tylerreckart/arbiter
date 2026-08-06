#include "repl/session.h"
#include "repl/session_internal.h"
#include "cli.h"
#include "cli_helpers.h"
#include "orchestrator.h"
#include "agent_conversation.h"
#include "commands.h"
#include "constitution.h"
#include "markdown.h"
#include "stream_renderer.h"
#include "render_policy.h"
#include "styled_text.h"
#include "api_server.h"
#include "tenant_store.h"
#include "scheduler.h"
#include "notification_bus.h"
#include "repl/queues.h"
#include "loop_manager.h"
#include "tui/tui.h"
#include "tui/tui_design.h"
#include "tui/stream_filter.h"
#include "tui/tty_guard.h"
#include "tui/confirm_keys.h"
#include "tui/interactive_prompt.h"
#include "tui/prompt_bridge.h"
#include "tui/sidebar.h"
#include "tui/history_sidebar.h"
#include "tui/theme_picker.h"
#include "tui/clipboard.h"
#include "tui/opentui/session.h"
#include "tui/opentui/sidebar_frame.h"
#include "tui/opentui/history_sidebar_frame.h"
#include "tui/opentui/theme_picker_frame.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/mouse_hit.h"
#include "repl/pane.h"
#include "repl/layout.h"
#include "repl/layout_snapshot.h"
#include "repl/pane_history.h"
#include "repl/repl_argv.h"
#include "repl/conversation_store.h"
#include "repl/conversation_titling.h"
#include "repl/transcript_replay.h"
#include "diff/apply.h"
#include "theme.h"
#include "config.h"

#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <csignal>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>

namespace fs = std::filesystem;

namespace arbiter {

void ReplSession::clear_spawn_parent_refs(Pane* parent) {

        if (!parent) return;
        layout_ptr->for_each_pane([&](Pane& p) {
            if (p.parent_pane == parent) p.parent_pane = nullptr;
        });
}

bool ReplSession::service_pending_closes() {

        std::vector<PendingClose> snapshot;
        {
            std::lock_guard<std::mutex> lk(pending_closes_mu);
            snapshot.swap(pending_closes);
        }
        if (snapshot.empty()) return false;

        for (auto& pc : snapshot) {
            // Verify the pane is still in the layout (user could have
            // Ctrl-w c'd it already).  If gone, skip silently.
            bool still_alive = false;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                layout_ptr->for_each_pane([&](Pane& p) {
                    if (&p == pc.pane) still_alive = true;
                });
            }
            if (!still_alive) continue;

            // Render the confirm prompt in the focused pane's scrollback.
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                StyledLine prompt_line;
                styled_append(prompt_line, StyleId::Warning,
                              "pane '" + pc.agent_id + "' finished — close it? [y/N]");
                pane_history_push_prose(layout_ptr->focused(), {prompt_line}, true);
                present_holding_lock();
            }

            const int key = arbiter::read_confirm_key();
            bool yes = (key == 'y' || key == 'Y');

            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                StyledLine answer;
                if (yes) {
                    styled_append(answer, StyleId::Success,
                                  "[closing '" + pc.agent_id + "']");
                } else {
                    styled_append(answer, StyleId::Error,
                                  "[keeping '" + pc.agent_id + "' open]");
                }
                pane_history_push_prose(layout_ptr->focused(), {answer}, true);
                present_holding_lock();
            }

            if (yes) {
                // Unblock confirm/diff waiters before join. Never join while
                // holding layout_mu — exec threads take that lock for /pane
                // spawn, /find, present_all, etc. (hang → SIGHUP/SIGSEGV in
                // pthread_join when the terminal is closed mid-wait).
                fail_pending_prompts();
                std::thread to_join;
                {
                    std::lock_guard<std::recursive_mutex> lk(layout_mu);
                    bool alive = false;
                    layout_ptr->for_each_pane([&](Pane& p) {
                        if (&p == pc.pane) alive = true;
                    });
                    if (!alive) continue;
                    clear_mouse_drag();
                    pc.pane->cmd_queue.stop();
                    // Docs: close cancels the in-flight turn so join returns
                    // promptly instead of waiting out a network call.
                    if (auto tok = std::atomic_load(&pc.pane->turn_cancel)) {
                        orch.cancel_token(tok);
                    }
                    to_join = std::move(pc.pane->exec_thread);
                }
                if (to_join.joinable()) to_join.join();
                {
                    std::lock_guard<std::recursive_mutex> lk(layout_mu);
                    bool alive = false;
                    layout_ptr->for_each_pane([&](Pane& p) {
                        if (&p == pc.pane) alive = true;
                    });
                    if (!alive) continue;
                    clear_mouse_drag();
                    clear_spawn_parent_refs(pc.pane);
                    layout_ptr->close_pane(pc.pane, [](Pane&) {});
                    g_getc_state.pane = &layout_ptr->focused();
                    ui_ctx.focused_pane = &layout_ptr->focused();
                    layout_ptr->for_each_pane([&](Pane& p) {
                        pane_history_set_cols(p, p.tui.cols());
                    });
                    persist_layout();
                    present_holding_lock();
                }
            }
        }
        return true;
}

void ReplSession::dispatch_chord(char cmd) {

        std::unique_lock<std::recursive_mutex> lk(layout_mu);
        clear_mouse_drag();
        switch (cmd) {
            case 'w':
            case 0x17:  // Ctrl-w Ctrl-w → next pane
                layout_ptr->focus_next();
                if (!layout_ptr->focused().conversation_id.empty()) {
                    conversation_store.set_active(layout_ptr->focused().conversation_id);
                    bind_tools_conversation(layout_ptr->focused().conversation_id);
                }
                break;
            case 's':
                if (sidebar.session_started()) {
                    sidebar.toggle_visible();
                    sync_layout_to_terminal();
                }
                break;
            case 'h':
                if (layout_ptr->pane_count() >= kMaxLayoutSnapshotLeaves) {
                    layout_ptr->focused().tui.set_status(
                        "pane cap reached (" +
                        std::to_string(kMaxLayoutSnapshotLeaves) +
                        ") — close one before splitting");
                } else if (Pane* np = layout_ptr->split_focused(
                        LayoutTree::Orient::Horizontal,
                        [this]() { return make_pane(); })) {
                    start_pane_thread(*np);
                }
                break;
            case 'v':
                if (layout_ptr->pane_count() >= kMaxLayoutSnapshotLeaves) {
                    layout_ptr->focused().tui.set_status(
                        "pane cap reached (" +
                        std::to_string(kMaxLayoutSnapshotLeaves) +
                        ") — close one before splitting");
                } else if (Pane* np = layout_ptr->split_focused(
                        LayoutTree::Orient::Vertical,
                        [this]() { return make_pane(); })) {
                    start_pane_thread(*np);
                }
                break;
            case 'c':
                if (layout_ptr->pane_count() > 1) {
                    // Unblock confirm/diff waiters before join.
                    fail_pending_prompts();
                    Pane* victim = &layout_ptr->focused();
                    victim->cmd_queue.stop();
                    // Match docs/tui keybindings: close cancels the in-flight
                    // turn so join is not stuck on a live network call.
                    if (auto tok = std::atomic_load(&victim->turn_cancel)) {
                        orch.cancel_token(tok);
                    }
                    std::thread to_join = std::move(victim->exec_thread);
                    lk.unlock();
                    if (to_join.joinable()) to_join.join();
                    lk.lock();
                    bool still_alive = false;
                    layout_ptr->for_each_pane([&](Pane& p) {
                        if (&p == victim) still_alive = true;
                    });
                    if (still_alive) {
                        clear_spawn_parent_refs(victim);
                        layout_ptr->close_pane(victim, [](Pane&) {});
                    }
                }
                break;
            case 'z':
                layout_ptr->toggle_zoom_focused();
                break;
            case 't':
                history_sidebar.toggle_enabled(dir);
                break;
            case 'b': {
                const int cols = arbiter::term_cols();
                if (HistorySidebarState::width_for_terminal(cols, true) <= 0) {
                    layout_ptr->focused().tui.set_status(
                        "History sidebar needs a wider terminal (>=72 cols) — try /chat list");
                    break;
                }
                if (!history_sidebar.enabled()) {
                    history_sidebar.set_enabled(true, dir);
                }
                enter_history_sidebar_focus();
                break;
            }
        }
        sync_layout_to_terminal();
        layout_ptr->for_each_pane([&](Pane& p) {
            pane_history_set_cols(p, p.tui.cols());
        });
        // Split / close / focus change the persisted tree (#42).
        persist_layout();
        present_unlocked();
        g_getc_state.pane = &layout_ptr->focused();
        ui_ctx.focused_pane = &layout_ptr->focused();
}

}  // namespace arbiter
