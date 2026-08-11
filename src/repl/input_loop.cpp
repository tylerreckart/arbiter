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
#include "tui/menu.h"
#include "tui/clipboard.h"
#include "tui/opentui/session.h"
#include "tui/opentui/sidebar_frame.h"
#include "tui/opentui/history_sidebar_frame.h"
#include "tui/opentui/menu_frame.h"
#include "model_catalog.h"
#include "model_context.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/mouse_hit.h"
#include "repl/pane.h"
#include "repl/layout.h"
#include "repl/layout_snapshot.h"
#include "repl/pane_history.h"
#include "repl/prompt_attachments.h"
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

void ReplSession::run_input_loop() {
    while (!quit_requested) {
        deferred_main_interrupt.store(false, std::memory_order_release);
        while (service_interactive()) {}
        while (service_pending_closes()) {}
        while (service_pending_conv_ops()) {}
        while (service_mouse_switch()) {}
        while (service_pending_after_cancel()) {}

        // A wake arrived while we were draining services (no active
        // read_line).  Re-enter so confirm/diff posts are not starved.
        if (deferred_main_interrupt.exchange(false, std::memory_order_acq_rel)) {
            continue;
        }

        if (overlay_menu.active()) {
            if (pump_notify) pump_notify();
            const auto menu_snap = overlay_menu.snapshot();
            const int visible_rows = arbiter::opentui::menu_visible_rows(
                layout_ptr->focused().tui,
                static_cast<int>(menu_snap.items.size()));
            const MenuPurpose purpose = menu_snap.purpose;

            char csi = 0;
            std::string csi_params;
            const int key = read_history_sidebar_key(csi, csi_params);
            if (key < 0) break;

            // Swallow mouse reports while the menu owns stdin.
            if (key == 0x1B && (csi == 'M' || csi == 'm')
                && !csi_params.empty() && csi_params[0] == '<') {
                continue;
            }

            auto preview_theme = [&]() {
                if (purpose != MenuPurpose::Theme) return;
                const auto item = overlay_menu.selected_item();
                if (item.id.empty() || item.section) return;
                // load_tui_design clears modal dim; re-apply so the menu
                // stays lifted over a recessed preview.
                arbiter::load_tui_design(dir, item.id);
                arbiter::tui_begin_modal_dim();
                refresh_chrome();
            };

            // Up / Down / Left / Right cycle (theme previews on move).
            if (key == 0x1B && (csi == 'A' || csi == 'D')) {
                overlay_menu.move_selection(-1, visible_rows);
                preview_theme();
                continue;
            }
            if (key == 0x1B && (csi == 'B' || csi == 'C')) {
                overlay_menu.move_selection(1, visible_rows);
                preview_theme();
                continue;
            }
            // PgUp / PgDn (CSI 5~ / 6~)
            if (key == 0x1B && csi == '~' && csi_params == "5") {
                overlay_menu.page_selection(-1, visible_rows);
                preview_theme();
                continue;
            }
            if (key == 0x1B && csi == '~' && csi_params == "6") {
                overlay_menu.page_selection(1, visible_rows);
                preview_theme();
                continue;
            }
            if (key == '\r' || key == '\n') {
                const auto item = overlay_menu.selected_item();
                const std::string ctx = overlay_menu.context();
                overlay_menu.close();
                tui_end_modal_dim();
                if (!item.id.empty() && !item.section) {
                    if (purpose == MenuPurpose::Theme) {
                        arbiter::set_tui_preset(dir, item.id);
                        refresh_chrome();
                        layout_ptr->focused().output_queue.push_prose_msg(
                            "theme: " + item.id, StyleId::System);
                    } else if (purpose == MenuPurpose::Model) {
                        const std::string agent_id =
                            ctx.empty() ? layout_ptr->focused().current_agent
                                        : ctx;
                        try {
                            orch.get_agent(agent_id).config_mut().model = item.id;
                            const int window = context_window_for_model(item.id);
                            std::string msg = agent_id + " model -> " + item.id
                                + "  ctx=" + format_context_window(window);
                            if (!find_model_catalog_entry(item.id)) {
                                msg += "\n  note: id not in catalogue";
                            }
                            layout_ptr->focused().output_queue.push_prose_msg(
                                msg, StyleId::System);
                        } catch (const std::exception& ex) {
                            layout_ptr->focused().output_queue.push_prose_msg(
                                "ERR: " + std::string(ex.what()), StyleId::Error);
                        }
                        refresh_chrome();
                    } else if (purpose == MenuPurpose::Help) {
                        // Prefer the agent /help corpus when a topic exists;
                        // otherwise echo the menu row's one-liner.
                        std::string detail =
                            orch.execute_slash_command("/help " + item.id,
                                                       layout_ptr->focused().current_agent);
                        if (detail.empty()
                            || detail.find("Unknown help topic") != std::string::npos) {
                            detail = item.label;
                            if (!item.detail.empty()) {
                                detail += "  — ";
                                detail += item.detail;
                            }
                            detail += "\n  Tip: type the command, or /help "
                                      + item.id + " for agent reference.";
                        }
                        layout_ptr->focused().output_queue.push_prose_msg(
                            detail, StyleId::System);
                        refresh_chrome();
                    }
                } else {
                    refresh_chrome();
                }
                continue;
            }
            if (key == 0x1B && csi == 0) {
                // Bare Esc — theme previews never wrote tui.json; restore disk.
                const bool was_theme = purpose == MenuPurpose::Theme;
                overlay_menu.close();
                tui_end_modal_dim();
                if (was_theme) arbiter::load_tui_design(dir);
                refresh_chrome();
                continue;
            }
            // Single-letter shortcuts (conversation-style menus).
            if (key >= 32 && key < 127) {
                if (overlay_menu.select_shortcut(static_cast<char>(key),
                                                 visible_rows)) {
                    preview_theme();
                }
                continue;
            }
            continue;
        }

        if (history_sidebar.focused()) {
            if (pump_notify) pump_notify();
            const int cols = arbiter::term_cols();
            const int rows = arbiter::term_rows();
            const Rect hb = HistorySidebarState::rect_for_terminal(cols, rows, true);
            const Rect outer = layout_ptr->outer_bounds();
            const int outer_bottom_pad =
                arbiter::tui_outer_bottom_pad_rows(arbiter::tui_design());
            const int visible_rows = arbiter::opentui::history_sidebar_visible_rows(
                hb, outer, outer_bottom_input_rows(), true, outer_bottom_pad);

            char csi = 0;
            std::string csi_params;
            const int key = read_history_sidebar_key(csi, csi_params);
            if (key < 0) break;

            // Mouse reports while the history sidebar owns stdin.
            if (key == 0x1B && (csi == 'M' || csi == 'm')
                && !csi_params.empty() && csi_params[0] == '<') {
                if (auto ev = arbiter::opentui::decode_sgr_mouse(csi_params, csi)) {
                    (void)route_mouse(*ev);
                }
                // Pane click exits history focus; queued history activation
                // is drained at the top of the next loop iteration.
                continue;
            }

            const HistorySidebarKey action = history_sidebar.handle_key(key, csi, csi_params);
            if (action == HistorySidebarKey::Up) {
                history_sidebar.move_selection(-1, visible_rows);
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::Down) {
                history_sidebar.move_selection(1, visible_rows);
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::Escape) {
                history_sidebar.exit_focus();
                tui_end_modal_dim();
                refresh_chrome();
                continue;
            }
            if (action == HistorySidebarKey::Enter) {
                switch_conversation(history_sidebar.is_new_selected(), {},
                                    history_sidebar.is_new_selected()
                                        ? history_sidebar.new_target_folder_id()
                                        : std::string{});
                continue;
            }
            if (action == HistorySidebarKey::New) {
                switch_conversation(true, {},
                                    history_sidebar.new_target_folder_id());
                continue;
            }
            if (action == HistorySidebarKey::ToggleFolder) {
                conversation_store.set_folder_collapse_json(
                    history_sidebar.collapse_json());
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::MoveStart) {
                tui_begin_modal_dim();
                refresh_chrome();
                continue;
            }
            if (action == HistorySidebarKey::MoveCommit) {
                const std::string cid = history_sidebar.selected_conversation_id();
                const std::string fid = history_sidebar.take_move_folder_id();
                if (is_remote()) {
                    layout_ptr->focused().tui.set_status(
                        "ERR: folders are not available in remote (--connect) mode");
                } else if (!cid.empty()) {
                    conversation_store.move_to_folder(cid, fid);
                }
                refresh_history_sidebar_entries();
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::PageUp) {
                history_sidebar.page_selection(-1, visible_rows);
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::PageDown) {
                history_sidebar.page_selection(1, visible_rows);
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::RenameStart) {
                if (history_sidebar.is_creating_folder()) {
                    tui_begin_modal_dim();
                    refresh_chrome();
                } else if (pump_notify) {
                    pump_notify();
                }
                continue;
            }
            if (action == HistorySidebarKey::MenuOpen) {
                tui_begin_modal_dim();
                refresh_chrome();
                continue;
            }
            if (action == HistorySidebarKey::RenameCommit) {
                const bool creating = history_sidebar.is_creating_folder();
                const bool target_folder = history_sidebar.rename_target_is_folder();
                const std::string target_id = history_sidebar.rename_target_id();
                const std::string text = history_sidebar.take_rename_buffer();
                if (!text.empty()) {
                    if (creating) {
                        if (is_remote()) {
                            layout_ptr->focused().tui.set_status(
                                "ERR: folders are not available in remote (--connect) mode");
                        } else {
                            const std::string fid =
                                conversation_store.create_folder(text);
                            refresh_history_sidebar_entries();
                            if (!fid.empty()) {
                                history_sidebar.select_folder(fid, visible_rows);
                            }
                        }
                        refresh_history_sidebar_entries();
                    } else if (target_folder) {
                        if (is_remote()) {
                            layout_ptr->focused().tui.set_status(
                                "ERR: folders are not available in remote (--connect) mode");
                        } else if (!target_id.empty()) {
                            conversation_store.rename_folder(target_id, text);
                        }
                        refresh_history_sidebar_entries();
                    } else if (!target_id.empty()) {
                        if (is_remote()) {
                            std::string err;
                            if (!remote->patch_conversation_title(target_id, text, &err)) {
                                layout_ptr->focused().tui.set_status(
                                    err.empty() ? "ERR: rename failed" : ("ERR: " + err));
                            }
                        } else {
                            conversation_store.set_title_locked(target_id, text);
                        }
                        refresh_history_sidebar_entries();
                    } else {
                        refresh_history_sidebar_entries();
                    }
                } else {
                    refresh_history_sidebar_entries();
                }
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::DeleteStart) {
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::DeleteConfirmed) {
                if (history_sidebar.is_folder_selected()) {
                    const std::string fid = history_sidebar.selected_folder_id();
                    if (is_remote()) {
                        layout_ptr->focused().tui.set_status(
                            "ERR: folders are not available in remote (--connect) mode");
                    } else if (!fid.empty()) {
                        conversation_store.delete_folder(fid);
                    }
                    refresh_history_sidebar_entries();
                    if (pump_notify) pump_notify();
                } else {
                    const std::string id = history_sidebar.selected_conversation_id();
                    if (!id.empty()) delete_conversation(id, /*hard=*/false);
                }
                continue;
            }
            // Rename typing, backspace, menu/move navigation, etc. return
            // None — still repaint so the inline buffer updates immediately.
            if (pump_notify) pump_notify();
            continue;
        }

        Pane& focused = layout_ptr->focused();
        ui_ctx.focused_pane = &focused;
        focused.tui.begin_input([&focused]() { return focused.cmd_queue.pending(); });

        std::string prompt = focused.multiline_accum.empty()
            ? focused.tui.build_prompt()
            : "\001" + theme().prompt_color + "\002"
                + arbiter::tui_design().component.continuation_prompt
                + "\001" + theme().reset + "\002";

        std::string line;
        active_readline.store(&focused.editor, std::memory_order_release);
        // Cover the gap between the deferred check above and publishing
        // active_readline: a wake that only set the flag (try_lock missed)
        // must still interrupt this read_line.
        if (deferred_main_interrupt.exchange(false, std::memory_order_acq_rel)) {
            focused.editor.interrupt();
        }
        const bool got_line = focused.editor.read_line(prompt, line);
        active_readline.store(nullptr, std::memory_order_release);
        if (!got_line) {
            char chord;
            if (focused.editor.take_chord(chord)) {
                dispatch_chord(chord);
                continue;
            }
            if (service_interactive()) continue;
            if (service_pending_closes()) continue;
            if (service_pending_conv_ops()) continue;
            if (service_mouse_switch()) continue;
            if (service_pending_after_cancel()) continue;
            if (overlay_menu.active()) continue;
            // Layout mutation woke us up just to repaint the focused
            // pane's prompt — loop back so begin_input paints a fresh
            // one.  Without this, read_line returning false here would
            // be treated as EOF and we'd exit.
            if (refresh_focused_input.exchange(false)) continue;
            if (deferred_main_interrupt.exchange(false)) continue;
            break;   // real EOF
        }
        if (quit_requested) break;

        // While a deferred switch/delete is waiting on cancel, keep the
        // input loop live but don't queue new turns onto the pane (#46).
        if (pending_after_cancel.kind != PendingAfterCancel::Kind::None) {
            if (service_pending_after_cancel()) continue;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                layout_ptr->focused().thinking.start("cancelling… (Esc to abort)");
                present_unlocked();
            }
            continue;
        }

        if (!line.empty()) focused.editor.add_to_history(line);

        focused.scroll_offset      = 0;
        focused.new_while_scrolled = 0;

        if (!line.empty() && line.back() == '\\') {
            focused.multiline_accum += line.substr(0, line.size() - 1) + "\n";
            continue;
        }
        line = focused.multiline_accum + line;
        focused.multiline_accum.clear();

        {
            std::string lower = line;
            for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
            while (!lower.empty() && lower.back()  == ' ') lower.pop_back();
            while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
            if (lower == "exit" || lower == "quit" || lower == "q" ||
                lower == "bye"  || lower == ":q"   ||
                lower == "/quit"|| lower == "/exit" || lower == "/q") {
                orch.cancel();
                layout_ptr->for_each_pane([&](Pane& p) { p.cmd_queue.drain(); });
                break;
            }
        }

        QueuedCommand queued;
        queued.text = line;
        // Slash commands keep staged images on the pane (/attach, /agents, …).
        // Only plain-text (and image-only) submits consume them into the turn.
        const bool slash = !line.empty() && line[0] == '/';
        if (!slash) {
            std::lock_guard<std::mutex> lk(focused.pending_attachments_mu);
            queued.attachments = std::move(focused.pending_attachments);
            focused.pending_attachments.clear();
            focused.tui.clear_status();
        }
        // Image-only submit: allow Enter with empty text when attachments
        // are staged; skip if another thread cleared them first.
        if (line.empty() && queued.attachments.empty()) continue;

        std::string echo = line;
        if (!queued.attachments.empty()) {
            if (!echo.empty()) echo += "\n";
            echo += "[" + attachment_status_label(queued.attachments) + "]";
        }
        focused.output_queue.push_prose(arbiter::styled_user_echo_lines(echo));
        focused.output_queue.end_message();

        focused.cmd_queue.push(std::move(queued));
        if (focused.cmd_queue.is_busy()) {
            focused.tui.show_queue_depth(focused.cmd_queue.pending());
            if (pump_notify) pump_notify();
        }
    }

}

}  // namespace arbiter
