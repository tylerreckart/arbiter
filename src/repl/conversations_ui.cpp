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

bool ReplSession::focused_turn_in_flight() {

        return layout_ptr->focused().cmd_queue.is_busy();
    
}

bool ReplSession::conversation_turn_in_flight(const std::string& id) {

        bool busy = false;
        layout_ptr->for_each_pane([&](Pane& p) {
            if (p.conversation_id == id && p.cmd_queue.is_busy()) busy = true;
        });
        return busy;
    
}

void ReplSession::apply_conversation_to_pane(Pane& pane, const std::string& id, bool replay) {

        clear_mouse_drag();
        pane.conversation_id = id;
        pane.current_agent = "index";
        pane.current_model = is_remote() ? std::string("remote")
                                         : orch.get_agent_model("index");
        pane.original_task.clear();
        pane.scroll_offset = 0;
        pane.new_while_scrolled = 0;
        pane.activity_unfocused.store(false);
        pane.completed_unfocused.store(false);
        pane.tui.clear_status();
        pane.tui.clear_activity_badge();
        pane_history_clear(pane);
        pane_history_set_cols(pane, pane.tui.cols());

        if (is_remote()) {
            conversation_set_active(id);
            if (replay && remote) {
                std::string err;
                auto msgs = remote->list_messages(id, &err, 200);
                // Render a simple user/assistant transcript into scrollback.
                for (const auto& m : msgs) {
                    if (m.role == "user") {
                        pane.output_queue.push_prose_msg(
                            m.content, StyleId::UserEchoText);
                        pane.output_queue.end_message();
                    } else if (m.role == "assistant") {
                        StreamRenderer renderer(kReplay, pane.output_queue);
                        renderer.feed(m.content);
                        renderer.flush();
                        pane.output_queue.end_message();
                    }
                }
            }
            refresh_history_sidebar_entries();
            return;
        }

        if (!orch.has_conversation_loaded(id)) {
            conversation_store.load(id, orch);
        }

        if (replay) {
            ConversationScope scope(id);
            const auto history = orch.get_agent_history("index");
            const size_t total = history.size();
            arbiter::replay_transcript(pane, history, arbiter::replay_tail_begin(total), total);
        }

        conversation_store.set_active(id);
        bind_tools_conversation(id);
    
}

void ReplSession::finish_switch_conversation(bool create_new, std::string explicit_id,
                                          std::string folder_id) {

        clear_mouse_drag();
        Pane& focused = layout_ptr->focused();
        focused.cmd_queue.drain();
        focused.tui.clear_queue_indicator();
        focused.thinking.stop();

        if (is_remote()) {
            if (create_new) {
                const std::string after = conversation_create(folder_id);
                if (after.empty() || after == focused.conversation_id) {
                    history_sidebar.exit_focus();
                    present_unlocked();
                    return;
                }
                history_sidebar.exit_focus();
                apply_conversation_to_pane(focused, after, /*replay=*/false);
                present_unlocked();
                g_getc_state.pane = &layout_ptr->focused();
                ui_ctx.focused_pane = &layout_ptr->focused();
                return;
            }
            const std::string picked = !explicit_id.empty() ? explicit_id
                                                             : history_sidebar.selected_conversation_id();
            if (picked.empty() || picked == focused.conversation_id) {
                history_sidebar.exit_focus();
                present_unlocked();
                return;
            }
            history_sidebar.exit_focus();
            apply_conversation_to_pane(focused, picked, /*replay=*/true);
            present_unlocked();
            g_getc_state.pane = &layout_ptr->focused();
            ui_ctx.focused_pane = &layout_ptr->focused();
            return;
        }

        conversation_store.flush();
        if (!focused.conversation_id.empty()) {
            conversation_store.save(focused.conversation_id, orch);
        }

        if (create_new) {
            const std::string before = focused.conversation_id;
            const std::string after = conversation_store.create_or_reuse_for(
                fs::current_path().string(), before, folder_id);
            if (after == before) {
                history_sidebar.exit_focus();
                present_unlocked();
                return;
            }
            {
                ConversationScope scope(after);
                orch.reset_all_histories();
            }
            history_sidebar.exit_focus();
            apply_conversation_to_pane(focused, after, /*replay=*/false);
            refresh_history_sidebar_entries();
            persist_layout();
            present_unlocked();
            g_getc_state.pane = &layout_ptr->focused();
            ui_ctx.focused_pane = &layout_ptr->focused();
            return;
        }

        const std::string picked = !explicit_id.empty() ? explicit_id
                                                         : history_sidebar.selected_conversation_id();
        if (picked.empty() || picked == focused.conversation_id) {
            history_sidebar.exit_focus();
            present_unlocked();
            return;
        }
        history_sidebar.exit_focus();
        apply_conversation_to_pane(focused, picked, /*replay=*/true);
        refresh_history_sidebar_entries();
        persist_layout();
        present_unlocked();
        g_getc_state.pane = &layout_ptr->focused();
        ui_ctx.focused_pane = &layout_ptr->focused();
    
}

void ReplSession::finish_delete_conversation(const std::string& id, bool hard, bool any_showing) {

        clear_mouse_drag();
        if (is_remote()) {
            if (any_showing) {
                layout_ptr->for_each_pane([&](Pane& p) {
                    if (p.conversation_id == id) {
                        p.cmd_queue.drain();
                        p.tui.clear_queue_indicator();
                    }
                });
                layout_ptr->focused().thinking.stop();
            }
            std::string err;
            if (!conversation_delete_remote(id, &err)) {
                layout_ptr->focused().tui.set_status(
                    err.empty() ? "ERR: delete failed" : ("ERR: " + err));
                present_unlocked();
                return;
            }
            const std::string replacement = [&]() {
                auto list = conversation_list();
                if (!list.empty()) return list.front().id;
                return conversation_create();
            }();
            layout_ptr->for_each_pane([&](Pane& p) {
                if (p.conversation_id == id) {
                    apply_conversation_to_pane(p, replacement, /*replay=*/true);
                }
            });
            present_unlocked();
            return;
        }

        if (any_showing) {
            layout_ptr->for_each_pane([&](Pane& p) {
                if (p.conversation_id == id) {
                    p.cmd_queue.drain();
                    p.tui.clear_queue_indicator();
                }
            });
            layout_ptr->focused().thinking.stop();
            conversation_store.flush();
            layout_ptr->for_each_pane([&](Pane& p) {
                if (p.conversation_id == id && !p.conversation_id.empty()) {
                    conversation_store.save(p.conversation_id, orch);
                }
            });
        }

        if (hard) conversation_store.purge(id);
        else conversation_store.soft_delete(id);

        orch.erase_conversation_histories(id);

        const std::string replacement = conversation_store.active_id();
        bool rebound = false;
        layout_ptr->for_each_pane([&](Pane& p) {
            if (p.conversation_id != id) return;
            apply_conversation_to_pane(p, replacement, /*replay=*/true);
            rebound = true;
        });

        refresh_history_sidebar_entries();
        if (rebound) {
            persist_layout();
            present_unlocked();
            g_getc_state.pane = &layout_ptr->focused();
            ui_ctx.focused_pane = &layout_ptr->focused();
        } else {
            present_unlocked();
        }
    
}

void ReplSession::begin_pending_after_cancel(PendingAfterCancel pending) {

        {
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            if (pending.kind == PendingAfterCancel::Kind::Switch && pending.pane) {
                auto token = std::atomic_load(&pending.pane->turn_cancel);
                if (token) {
                    orch.cancel_token(token);
                } else {
                    orch.cancel();
                }
            } else if (pending.kind == PendingAfterCancel::Kind::Delete) {
                bool cancelled_any = false;
                layout_ptr->for_each_pane([&](Pane& p) {
                    if (p.conversation_id != pending.wait_conversation_id) return;
                    auto token = std::atomic_load(&p.turn_cancel);
                    if (token) {
                        orch.cancel_token(token);
                        cancelled_any = true;
                    }
                });
                if (!cancelled_any) orch.cancel();
            }
            history_sidebar.exit_focus();
            layout_ptr->focused().thinking.start("cancelling… (Esc to abort)");
            present_unlocked();
        }
        pending_after_cancel = std::move(pending);
        pending_cancel_wait.store(true);
    
}

bool ReplSession::service_pending_after_cancel() {

        if (pending_after_cancel.kind == PendingAfterCancel::Kind::None) return false;

        std::unique_lock<std::recursive_mutex> lk(layout_mu);
        bool busy = false;
        if (pending_after_cancel.kind == PendingAfterCancel::Kind::Switch) {
            Pane* pane = pending_after_cancel.pane;
            bool alive = false;
            if (pane) {
                layout_ptr->for_each_pane([&](Pane& p) {
                    if (&p == pane) alive = true;
                });
            }
            busy = alive && pane->cmd_queue.is_busy();
        } else {
            busy = conversation_turn_in_flight(pending_after_cancel.wait_conversation_id);
        }

        if (busy) {
            layout_ptr->focused().thinking.start("cancelling… (Esc to abort)");
            layout_ptr->focused().thinking.tick();
            return false;
        }

        const auto op = pending_after_cancel;
        pending_after_cancel = {};
        pending_cancel_wait.store(false);

        if (op.kind == PendingAfterCancel::Kind::Switch) {
            finish_switch_conversation(op.create_new, op.target_id, op.folder_id);
        } else if (op.kind == PendingAfterCancel::Kind::Delete) {
            finish_delete_conversation(op.target_id, op.hard_delete, /*any_showing=*/true);
        }
        return true;
    
}

void ReplSession::switch_conversation(bool create_new, std::string explicit_id,
                                   std::string folder_id) {

        if (pending_after_cancel.kind != PendingAfterCancel::Kind::None) {
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            layout_ptr->focused().tui.set_status("Already cancelling… (Esc to abort)");
            present_unlocked();
            return;
        }

        // Confirm outside layout_mu so the output pump can keep painting and
        // so nested mouse Up reports are drained by read_confirm_key.
        {
            bool busy = false;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                busy = focused_turn_in_flight();
                if (busy) {
                    layout_ptr->focused().tui.set_status(
                        "Turn in progress — switch anyway? [y/N]");
                    present_unlocked();
                }
            }
            if (busy) {
                const int key = arbiter::read_confirm_key();
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                layout_ptr->focused().tui.clear_status();
                if (key != 'y' && key != 'Y') {
                    history_sidebar.exit_focus();
                    present_unlocked();
                    return;
                }
            }
        }

        {
            bool need_cancel = false;
            Pane* pane = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                need_cancel = focused_turn_in_flight();
                pane = &layout_ptr->focused();
            }
            if (need_cancel) {
                PendingAfterCancel pending;
                pending.kind = PendingAfterCancel::Kind::Switch;
                pending.create_new = create_new;
                pending.target_id = std::move(explicit_id);
                pending.folder_id = std::move(folder_id);
                pending.pane = pane;
                pending.abandon_status = "Switch cancelled";
                begin_pending_after_cancel(std::move(pending));
                return;
            }
        }

        std::unique_lock<std::recursive_mutex> lk(layout_mu);
        finish_switch_conversation(create_new, std::move(explicit_id),
                                   std::move(folder_id));
    
}

void ReplSession::delete_conversation(const std::string& id, bool hard) {

        if (pending_after_cancel.kind != PendingAfterCancel::Kind::None) {
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            layout_ptr->focused().tui.set_status("Already cancelling… (Esc to abort)");
            present_unlocked();
            return;
        }

        bool any_showing = false;
        bool need_cancel = false;
        {
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            layout_ptr->for_each_pane([&](Pane& p) {
                if (p.conversation_id == id) any_showing = true;
            });
            if (any_showing) need_cancel = conversation_turn_in_flight(id);
        }

        if (need_cancel) {
            PendingAfterCancel pending;
            pending.kind = PendingAfterCancel::Kind::Delete;
            pending.target_id = id;
            pending.hard_delete = hard;
            pending.wait_conversation_id = id;
            pending.abandon_status = "Delete cancelled";
            begin_pending_after_cancel(std::move(pending));
            return;
        }

        std::unique_lock<std::recursive_mutex> lk(layout_mu);
        finish_delete_conversation(id, hard, any_showing);
    
}

bool ReplSession::service_pending_conv_ops() {

        std::vector<PendingConversationOp> snapshot;
        {
            std::lock_guard<std::mutex> lk(pending_conv_mu);
            snapshot.swap(pending_conv_ops);
        }
        if (snapshot.empty()) return false;

        for (auto& op : snapshot) {
            if (op.switch_op) {
                switch_conversation(op.create_new, op.target_id, op.folder_id);
            } else if (op.delete_op) {
                delete_conversation(op.target_id, op.hard_delete);
            }
        }
        return true;
    
}

}  // namespace arbiter
