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

void ReplSession::start_pane_thread(Pane& p_ref) {

        Pane* pane_ptr = &p_ref;
        pane_ptr->exec_thread = std::thread([this, pane_ptr]() {
            Pane& p = *pane_ptr;
            g_active_pane = &p;
            // /parallel workers capture this binder by value at spawn time.
            {
                const int64_t cid =
                    parse_conversation_db_id(p.conversation_id);
                orch.set_worker_pane_binder([pane_ptr, cid]() {
                    g_active_pane = pane_ptr;
                    arbiter::set_tool_conversation_tls(cid);
                });
            }
            std::string line;
            while (p.cmd_queue.pop(line)) {
                auto turn_token = std::make_shared<arbiter::CancelToken>();
                std::atomic_store(&p.turn_cancel, turn_token);
                arbiter::RequestCancelScope cancel_scope(orch.client(), turn_token);
                p.cmd_queue.set_busy(true);
                p.turn_running.store(true);
                p.last_response.clear();
                // Re-install each turn so a sibling pane that also ran
                // /parallel can't leave a stale binder in place.
                {
                    const int64_t cid =
                        parse_conversation_db_id(p.conversation_id);
                    orch.set_worker_pane_binder([pane_ptr, cid]() {
                        g_active_pane = pane_ptr;
                        arbiter::set_tool_conversation_tls(cid);
                    });
                }
                if (layout_ptr && &layout_ptr->focused() != &p) {
                    p.activity_unfocused.store(true);
                    p.tui.set_activity_badge("●");
                }
                {
                    ConversationScope scope(p.conversation_id);
                    bind_tools_conversation(p.conversation_id);
                    handle_line(p, line);
                }
                p.turn_running.store(false);
                // Only latch a completion badge when this turn actually ran an
                // agent response (last_response set). Slash-only commands skip.
                const bool had_agent_turn = !p.last_response.empty();
                const bool ok = p.last_response.rfind("ERR:", 0) != 0;
                p.last_turn_ok.store(ok);
                if (layout_ptr && &layout_ptr->focused() != &p) {
                    if (had_agent_turn) {
                        p.completed_unfocused.store(true);
                        p.tui.set_activity_badge(ok ? "✓" : "✗");
                    }
                } else {
                    p.completed_unfocused.store(false);
                    p.tui.clear_activity_badge();
                }
                p.cmd_queue.set_busy(false);
                p.tui.clear_queue_indicator();
                std::atomic_store(&p.turn_cancel, std::shared_ptr<arbiter::CancelToken>{});
                // Wake the main loop if a deferred switch/delete is waiting
                // for this turn to unwind (#46).
                if (pending_cancel_wait.load()) {
                    wake_main_input();
                }

                if (p.parent_pane != nullptr &&
                    !p.spawn_flowed.exchange(true)) {

                    Pane* parent = p.parent_pane;
                    {
                        // Hold layout_mu across the alive check *and* the
                        // push: close can destroy `parent` the instant we
                        // unlock, and cmd_queue lives on the Pane.
                        std::lock_guard<std::recursive_mutex> lk(layout_mu);
                        bool parent_alive = false;
                        layout_ptr->for_each_pane([&](Pane& other) {
                            if (&other == parent) parent_alive = true;
                        });
                        if (parent_alive) {
                            std::string task_preview = p.spawn_message.size() > 80
                                ? p.spawn_message.substr(0, 77) + "..."
                                : p.spawn_message;
                            std::string frame = "[PANE RESULT from '"
                                + p.current_agent + "' (task: "
                                + task_preview + ")]\n"
                                + p.last_response
                                + "\n[END PANE RESULT]";
                            parent->cmd_queue.push(frame);
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lk(pending_closes_mu);
                        pending_closes.push_back({&p, p.current_agent});
                    }
                    if (layout_ptr) wake_main_input();
                }
            }
        });
}

void ReplSession::start_output_pump() {
    pump_notify = [this]() {
        { std::lock_guard<std::mutex> lk(pump_cv_mu); pump_notified = true; }
        pump_cv.notify_one();
    };

    // Start exec threads after pump_notify is assigned — each exec thread
    // captures pump_notify by reference via OutputQueue::notify_fn_ and
    // may call push() on its first tick.  Restored multi-pane layouts need
    // a thread per leaf, not only the focused pane.
    layout_ptr->for_each_pane([&](Pane& p) { start_pane_thread(p); });

    pump_stop.store(false);
    output_pump = std::thread([this]() {
        auto push_pane_output = [this](Pane& p) {
            const int before = pane_history_total_rows(p);
            pane_history_drain_queue(p);
            // New output on an unfocused pane → activity pulse (#41).
            if (&p != &layout_ptr->focused()
                && pane_history_total_rows(p) > before
                && !p.turn_running.load()) {
                p.activity_unfocused.store(true);
                if (!p.completed_unfocused.load()) {
                    p.tui.set_activity_badge("●");
                }
            }
        };
        auto present_all = [this]() { present_unlocked(); };
        while (!pump_stop.load()) {
            {
                std::unique_lock<std::mutex> wlk(pump_cv_mu);
                pump_cv.wait_for(wlk, std::chrono::milliseconds(30),
                                 [&]{ return pump_notified || pump_stop.load(); });
                pump_notified = false;
            }
            std::unique_lock<std::recursive_mutex> lk(layout_mu);
            (void)consume_sigwinch();
            if (sync_layout_to_terminal()) {
                g_getc_state.pane = &layout_ptr->focused();
                ui_ctx.focused_pane = &layout_ptr->focused();
                refresh_focused_input.store(true);
                layout_ptr->focused().editor.interrupt();
            }
            layout_ptr->for_each_pane(push_pane_output);
            present_all();
        }
        layout_ptr->for_each_pane(push_pane_output);
        present_all();
    });
}

}  // namespace arbiter
