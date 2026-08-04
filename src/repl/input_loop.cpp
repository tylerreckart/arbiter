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

        if (theme_picker.active()) {
            if (pump_notify) pump_notify();
            const int visible_rows =
                arbiter::opentui::theme_picker_visible_rows(layout_ptr->focused().tui);

            char csi = 0;
            std::string csi_params;
            const int key = read_history_sidebar_key(csi, csi_params);
            if (key < 0) break;

            // Swallow mouse reports while the picker owns stdin.
            if (key == 0x1B && (csi == 'M' || csi == 'm')
                && !csi_params.empty() && csi_params[0] == '<') {
                continue;
            }

            auto preview_selected = [&]() {
                const std::string name = theme_picker.selected_theme();
                if (name.empty()) return;
                arbiter::load_tui_design(dir, name);
                refresh_chrome();
            };

            // Up / Down / Left / Right cycle with live preview.
            if (key == 0x1B && (csi == 'A' || csi == 'D')) {
                theme_picker.move_selection(-1, visible_rows);
                preview_selected();
                continue;
            }
            if (key == 0x1B && (csi == 'B' || csi == 'C')) {
                theme_picker.move_selection(1, visible_rows);
                preview_selected();
                continue;
            }
            // PgUp / PgDn (CSI 5~ / 6~)
            if (key == 0x1B && csi == '~' && csi_params == "5") {
                theme_picker.page_selection(-1, visible_rows);
                preview_selected();
                continue;
            }
            if (key == 0x1B && csi == '~' && csi_params == "6") {
                theme_picker.page_selection(1, visible_rows);
                preview_selected();
                continue;
            }
            if (key == '\r' || key == '\n') {
                const std::string name = theme_picker.selected_theme();
                theme_picker.close();
                if (!name.empty()) {
                    arbiter::set_tui_preset(dir, name);
                    refresh_chrome();
                    layout_ptr->focused().output_queue.push_prose_msg(
                        "theme: " + name, StyleId::System);
                }
                if (pump_notify) pump_notify();
                continue;
            }
            if (key == 0x1B && csi == 0) {
                // Bare Esc — restore disk theme (previews never wrote tui.json).
                theme_picker.close();
                arbiter::load_tui_design(dir);
                refresh_chrome();
                if (pump_notify) pump_notify();
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
                if (pump_notify) pump_notify();
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
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::MoveCommit) {
                const std::string cid = history_sidebar.selected_conversation_id();
                const std::string fid = history_sidebar.take_move_folder_id();
                if (!cid.empty()) {
                    conversation_store.move_to_folder(cid, fid);
                }
                history_sidebar.refresh_entries(conversation_store);
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
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::MenuOpen) {
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::RenameCommit) {
                const bool creating = history_sidebar.is_creating_folder();
                const bool target_folder = history_sidebar.rename_target_is_folder();
                const std::string target_id = history_sidebar.rename_target_id();
                const std::string text = history_sidebar.take_rename_buffer();
                if (!text.empty()) {
                    if (creating) {
                        const std::string fid =
                            conversation_store.create_folder(text);
                        history_sidebar.refresh_entries(conversation_store);
                        if (!fid.empty()) {
                            history_sidebar.select_folder(fid, visible_rows);
                        }
                    } else if (target_folder) {
                        if (!target_id.empty()) {
                            conversation_store.rename_folder(target_id, text);
                        }
                        history_sidebar.refresh_entries(conversation_store);
                    } else if (!target_id.empty()) {
                        conversation_store.set_title_locked(target_id, text);
                        history_sidebar.refresh_entries(conversation_store);
                    } else {
                        history_sidebar.refresh_entries(conversation_store);
                    }
                } else {
                    history_sidebar.refresh_entries(conversation_store);
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
                    if (!fid.empty()) conversation_store.delete_folder(fid);
                    history_sidebar.refresh_entries(conversation_store);
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
            if (theme_picker.active()) continue;
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

        if (line.empty()) continue;

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

        focused.output_queue.push_prose(arbiter::styled_user_echo_lines(line));
        focused.output_queue.end_message();

        focused.cmd_queue.push(line);
        if (focused.cmd_queue.is_busy()) {
            focused.tui.show_queue_depth(focused.cmd_queue.pending());
            if (pump_notify) pump_notify();
        }
    }

}

}  // namespace arbiter
