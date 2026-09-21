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
#include "tui/opentui/fleet_frame.h"
#include "tui/opentui/menu_frame.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/mouse_hit.h"
#include "tui/fleet.h"
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

void ReplSession::clear_mouse_select() {
 mouse_select = {};
}

void ReplSession::clear_mouse_select_and_highlight() {

        if (mouse_select.pane) pane_history_clear_selection(*mouse_select.pane);
        mouse_select = {};
}

void ReplSession::clear_mouse_drag() {

        mouse_drag = {};
        clear_mouse_select_and_highlight();
}

void ReplSession::clear_all_selections() {

        if (!layout_ptr) return;
        layout_ptr->for_each_pane([&](Pane& p) {
            pane_history_clear_selection(p);
        });
}

void ReplSession::scroll_pane(Pane& pane, int direction, int step) {

        const int max_off = pane_history_max_scroll(pane);
        if (direction < 0) {
            pane.scroll_offset = std::min(pane.scroll_offset + step, max_off);
            if (pane.scroll_offset >= max_off && pane.scroll && pane.scroll->has_gap()) {
                ConversationScope scope(pane.conversation_id);
                const std::string& agent = pane.current_agent.empty()
                    ? "index" : pane.current_agent;
                const auto history = orch.get_agent_history(agent);
                arbiter::replay_load_previous_chunk(pane, history);
            }
        } else {
            pane.scroll_offset = std::max(0, pane.scroll_offset - step);
            pane.new_while_scrolled = 0;
            if (pane.scroll_offset == 0) pane.tui.clear_status();
        }
        if (pump_notify) pump_notify();
}

Rect ReplSession::right_sidebar_rect() {

        const int cols = arbiter::term_cols();
        const int rows = arbiter::term_rows();
        const int panes = static_cast<int>(layout_ptr->pane_count());
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        const int sw = sidebar.effective_width(cols, panes, leading);
        if (sw <= 0) return {};
        const int pane_x = layout_ptr->outer_bounds().x;
        const int pane_w = layout_ptr->outer_bounds().w;
        const int gap = cols - pane_x - pane_w;
        if (gap < sw) return {};
        Rect rail{pane_x + pane_w, 0, sw, std::max(1, rows)};
        const Rect fleet = fleet_sidebar_rect();
        if (fleet.w > 0 && fleet.h > 0 && fleet.h < rows) {
            rail.y = fleet.y + fleet.h;
            rail.h = std::max(1, rows - rail.y);
        }
        return rail;
}

bool ReplSession::fleet_has_content() const {
    return !fleet.empty();
}

Rect ReplSession::fleet_sidebar_rect() {
        const int cols = arbiter::term_cols();
        const int rows = arbiter::term_rows();
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        const int fw = fleet_sidebar.effective_width(
            cols, leading, fleet_has_content() || fleet_sidebar.focused());
        if (fw <= 0) return {};
        const int pane_x = layout_ptr->outer_bounds().x;
        const int pane_w = layout_ptr->outer_bounds().w;
        const int gap = cols - pane_x - pane_w;
        if (gap < fw) return {};
        Rect rail{pane_x + pane_w, 0, fw, std::max(1, rows)};
        const int panes = static_cast<int>(layout_ptr->pane_count());
        const int sw = sidebar.effective_width(cols, panes, leading);
        if (sw > 0 && rows > 16) {
            // Stack fleet above the session box so neither drowns the other.
            rail.h = std::max(10, rows * 2 / 5);
        }
        return rail;
}

int ReplSession::fleet_visible_rows(const Rect& fb, const FleetSnapshot& snap) {
        const Rect outer = layout_ptr->outer_bounds();
        const int outer_bottom_pad =
            arbiter::tui_outer_bottom_pad_rows(arbiter::tui_design());
        return arbiter::opentui::fleet_sidebar_visible_rows(
            fb, outer, outer_bottom_input_rows(), snap.overlay_lines,
            outer_bottom_pad);
}

void ReplSession::enter_fleet_focus() {
        if (history_sidebar.focused()) history_sidebar.exit_focus();
        fleet_sidebar.enter_focus();
}

Pane* ReplSession::pane_for_fleet_row(const FleetPaintRow& row) {
        if (!layout_ptr) return nullptr;
        Pane* host = nullptr;
        Pane* agent_match = nullptr;
        layout_ptr->for_each_pane([&](Pane& p) {
            if (!row.conversation_id.empty()
                && p.conversation_id != row.conversation_id) {
                return;
            }
            if (!host) host = &p;
            if (!row.agent.empty() && p.current_agent == row.agent) {
                agent_match = &p;
            } else if (!row.pane_agent.empty()
                       && p.current_agent == row.pane_agent && !agent_match) {
                host = &p;
            }
        });
        if (agent_match) return agent_match;
        return host ? host : &layout_ptr->focused();
}

bool ReplSession::steer_fleet_selection() {
        const int cols = arbiter::term_cols();
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        const auto snap = fleet_sidebar.snapshot(fleet, cols, leading);
        if (snap.rows.empty()) return false;
        int idx = snap.selected;
        if (idx < 0 || idx >= static_cast<int>(snap.rows.size())) return false;
        Pane* target = pane_for_fleet_row(snap.rows[static_cast<size_t>(idx)]);
        fleet_sidebar.exit_focus();
        if (target && layout_ptr) {
            layout_ptr->focus_pane(target);
            if (!target->conversation_id.empty()) {
                conversation_store.set_active(target->conversation_id);
                bind_tools_conversation(target->conversation_id);
            }
            g_getc_state.pane = target;
            ui_ctx.focused_pane = target;
        }
        refresh_focused_input.store(true);
        if (pump_notify) pump_notify();
        return true;
}

bool ReplSession::cancel_fleet_selection() {
        const int cols = arbiter::term_cols();
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        const auto snap = fleet_sidebar.snapshot(fleet, cols, leading);
        if (snap.rows.empty()) return false;
        int idx = snap.selected;
        if (idx < 0 || idx >= static_cast<int>(snap.rows.size())) return false;
        const auto& row = snap.rows[static_cast<size_t>(idx)];
        if (!row.running) return false;
        Pane* target = pane_for_fleet_row(row);
        if (target) cancel_pane_turn(*target);
        return true;
}

int ReplSession::history_visible_rows(const Rect& hb) {

        const Rect outer = layout_ptr->outer_bounds();
        const int outer_bottom_pad =
            arbiter::tui_outer_bottom_pad_rows(arbiter::tui_design());
        return arbiter::opentui::history_sidebar_visible_rows(
            hb, outer, outer_bottom_input_rows(), history_sidebar.focused(),
            outer_bottom_pad);
}

bool ReplSession::route_mouse(const opentui::MouseEvent& ev) {
        using arbiter::opentui::HitKind;
        using arbiter::opentui::MouseButton;
        using arbiter::opentui::MouseType;
        using arbiter::opentui::ScrollDir;

        if (!arbiter::tui_design().layout.mouse) return false;

        std::lock_guard<std::recursive_mutex> lk(layout_mu);

        // Separator drag: re-resolve the path-based ref each event so a
        // mid-drag tree mutation fails cleanly instead of UAFing.
        if (mouse_drag.active) {
            if (ev.type == MouseType::Up) {
                mouse_drag.active = false;
                // Persist asymmetric weights after a completed drag (#42).
                persist_layout();
                return false;
            }
            if (ev.type == MouseType::Drag && ev.button == MouseButton::Left) {
                if (!layout_ptr->drag_separator(mouse_drag.sep, ev.x, ev.y)) {
                    mouse_drag.active = false;
                    persist_layout();
                } else {
                    layout_ptr->for_each_pane([&](Pane& p) {
                        pane_history_set_cols(p, p.tui.cols());
                    });
                    if (pump_notify) pump_notify();
                }
                return false;
            }
            if (ev.type == MouseType::Move) return false;
            // Any other event cancels an in-progress drag.
            mouse_drag.active = false;
            persist_layout();
        }

        // Output text selection drag — continue even if the pointer leaves
        // the scroll band so the range can still grow / finish.
        if (mouse_select.active && mouse_select.pane) {
            if (ev.type == MouseType::Drag && ev.button == MouseButton::Left) {
                if (auto cell = pane_history_hit_cell_at(
                        *mouse_select.pane, ev.x, ev.y)) {
                    if (*cell != mouse_select.anchor) mouse_select.dragged = true;
                    pane_history_set_selection(
                        *mouse_select.pane, mouse_select.anchor, *cell);
                } else {
                    // Outside content: clamp focus to the nearest edge of the
                    // selection pane's last hit by keeping the prior focus.
                    mouse_select.dragged = true;
                }
                if (pump_notify) pump_notify();
                return false;
            }
            if (ev.type == MouseType::Up) {
                Pane* pane = mouse_select.pane;
                const bool dragged = mouse_select.dragged;
                const auto anchor = mouse_select.anchor;
                clear_mouse_select();
                if (dragged) {
                    if (auto cell = pane_history_hit_cell_at(*pane, ev.x, ev.y)) {
                        pane_history_set_selection(*pane, anchor, *cell);
                    }
                    const std::string text = pane_history_selection_text(*pane);
                    if (!text.empty()) {
                        if (arbiter::clipboard_write_osc52(text)) {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf),
                                          "copied %zu character%s",
                                          text.size(),
                                          text.size() == 1 ? "" : "s");
                            pane->tui.set_status(buf);
                        }
                    }
                    if (pump_notify) pump_notify();
                    return false;
                }
                // Click (no drag): clear any prior selection and toggle
                // expand/collapse on the expandable under the pointer.
                pane_history_clear_selection(*pane);
                const bool toggled =
                    pane_history_toggle_expandable_at(*pane, ev.x, ev.y);
                if (toggled && pump_notify) pump_notify();
                else if (pump_notify) pump_notify();
                return false;
            }
            if (ev.type == MouseType::Move) return false;
            // Other events cancel an in-progress select gesture.
            clear_mouse_select_and_highlight();
        }

        const int cols = arbiter::term_cols();
        const int rows = arbiter::term_rows();
        const Rect hb = HistorySidebarState::rect_for_terminal(
            cols, rows, history_sidebar.enabled());
        const Rect fb = fleet_sidebar_rect();
        const Rect rb = right_sidebar_rect();
        const int hist_vis = (hb.w > 0) ? history_visible_rows(hb) : 0;
        const auto hist_snap = (hb.w > 0)
            ? history_sidebar.snapshot()
            : arbiter::HistorySidebarSnapshot{};
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        const auto fleet_snap = (fb.w > 0)
            ? fleet_sidebar.snapshot(fleet, cols, leading)
            : arbiter::FleetSnapshot{};
        const int fleet_vis = (fb.w > 0) ? fleet_visible_rows(fb, fleet_snap) : 0;
        const auto hit = arbiter::opentui::hit_test(
            *layout_ptr, hb, rb, hist_snap.scroll_offset, hist_vis, hist_snap.rows,
            ev.x, ev.y, fb, fleet_snap.scroll_offset, fleet_vis,
            fleet_snap.overlay_lines, static_cast<int>(fleet_snap.rows.size()));

        if (ev.type == MouseType::Scroll) {
            const int dir = (ev.scroll == ScrollDir::Up || ev.scroll == ScrollDir::Left)
                ? -1 : +1;
            if (hit.kind == HitKind::HistorySidebar && hb.w > 0) {
                history_sidebar.page_selection(dir, hist_vis);
                if (pump_notify) pump_notify();
                return false;
            }
            if (hit.kind == HitKind::FleetSidebar && fb.w > 0) {
                fleet_sidebar.page_selection(dir, fleet_vis);
                if (pump_notify) pump_notify();
                return false;
            }
            // Only scroll when the pointer is over a pane scroll/input/chrome
            // region — never fall back to the focused pane for Outside /
            // RightSidebar / FleetSidebar / separator hits.
            if (hit.pane
                && (hit.kind == HitKind::PaneScroll
                    || hit.kind == HitKind::PaneInput
                    || hit.kind == HitKind::PaneChrome)) {
                const int step = std::max(1, hit.pane->tui.scroll_region_rows() / 4);
                scroll_pane(*hit.pane, dir, step * std::max(1, ev.scroll_delta));
            }
            return false;
        }

        if (ev.type == MouseType::Down && ev.button == MouseButton::Left) {
            if (hit.kind == HitKind::SplitSeparator) {
                clear_mouse_select();
                clear_all_selections();
                mouse_drag.active = true;
                mouse_drag.sep = hit.sep;
                if (pump_notify) pump_notify();
                return false;
            }
            if (hit.kind == HitKind::HistorySidebar) {
                clear_mouse_select();
                clear_all_selections();
                if (!history_sidebar.focused()) {
                    enter_history_sidebar_focus();
                }
                if (hit.history_row >= 0) {
                    history_sidebar.select_at_index(hit.history_row, hist_vis);
                    // Queue activation — never call switch_conversation from
                    // inside the mouse handler (nested stdin confirm + lock).
                    if (history_sidebar.is_folder_selected()) {
                        mouse_switch.pending = true;
                        mouse_switch.toggle_folder = true;
                        mouse_switch.create_new = false;
                        mouse_switch.folder_id.clear();
                    } else {
                        mouse_switch.pending = true;
                        mouse_switch.toggle_folder = false;
                        mouse_switch.create_new = history_sidebar.is_new_selected();
                        mouse_switch.folder_id = mouse_switch.create_new
                            ? history_sidebar.new_target_folder_id()
                            : std::string{};
                    }
                }
                refresh_focused_input.store(true);
                if (pump_notify) pump_notify();
                return true;
            }
            if (hit.kind == HitKind::FleetSidebar) {
                clear_mouse_select();
                clear_all_selections();
                if (history_sidebar.focused()) history_sidebar.exit_focus();
                if (!fleet_sidebar.focused()) enter_fleet_focus();
                if (hit.fleet_row >= 0) {
                    fleet_sidebar.select_at_index(hit.fleet_row, fleet_vis);
                    mouse_fleet.pending = true;
                    mouse_fleet.cancel = false;
                }
                refresh_focused_input.store(true);
                if (pump_notify) pump_notify();
                return true;
            }
            if (hit.kind == HitKind::RightSidebar) {
                // Display-only telemetry panel — clicks are intentionally
                // ignored (documented in docs/tui/panes.md).
                return false;
            }
            if ((hit.kind == HitKind::PaneInput
                 || hit.kind == HitKind::PaneScroll
                 || hit.kind == HitKind::PaneChrome)
                && hit.pane) {
                const bool focus_changed = (hit.pane != layout_ptr->focused_ptr());
                const bool was_history = history_sidebar.focused();
                const bool was_fleet = fleet_sidebar.focused();
                if (was_history) history_sidebar.exit_focus();
                if (was_fleet) fleet_sidebar.exit_focus();
                layout_ptr->focus_pane(hit.pane);
                if (hit.kind == HitKind::PaneInput
                    || hit.kind == HitKind::PaneChrome) {
                    clear_mouse_select();
                    clear_all_selections();
                    if (hit.kind == HitKind::PaneInput) {
                        hit.pane->editor.set_cursor_from_click(ev.x, ev.y);
                    }
                    if (focus_changed || was_history || was_fleet) {
                        refresh_focused_input.store(true);
                        if (pump_notify) pump_notify();
                        return focus_changed || was_history || was_fleet;
                    }
                    if (pump_notify) pump_notify();
                    return false;
                }
                // PaneScroll: start a selection gesture. Expand/collapse is
                // deferred to Up so a drag can select without toggling.
                clear_all_selections();
                if (auto cell = pane_history_hit_cell_at(*hit.pane, ev.x, ev.y)) {
                    mouse_select.active = true;
                    mouse_select.dragged = false;
                    mouse_select.pane = hit.pane;
                    mouse_select.anchor = *cell;
                    pane_history_set_selection(*hit.pane, *cell, *cell);
                } else {
                    clear_mouse_select();
                }
                if (focus_changed || was_history || was_fleet) {
                    refresh_focused_input.store(true);
                    if (pump_notify) pump_notify();
                    return focus_changed || was_history || was_fleet;
                }
                if (pump_notify) pump_notify();
                return false;
            }
            // Outside / unknown: drop any selection.
            clear_mouse_select();
            clear_all_selections();
            if (pump_notify) pump_notify();
        }

        return false;
}

bool ReplSession::service_mouse_switch() {

        if (!mouse_switch.pending) return false;
        const bool create_new = mouse_switch.create_new;
        const bool toggle_folder = mouse_switch.toggle_folder;
        const std::string folder_id = mouse_switch.folder_id;
        mouse_switch.pending = false;
        mouse_switch.toggle_folder = false;
        mouse_switch.folder_id.clear();
        if (toggle_folder) {
            // Simulate Enter on the folder header to toggle collapse.
            const auto action = history_sidebar.handle_key('\r', 0, "");
            if (action == HistorySidebarKey::ToggleFolder) {
                conversation_store.set_folder_collapse_json(
                    history_sidebar.collapse_json());
            }
            if (pump_notify) pump_notify();
            return true;
        }
        switch_conversation(create_new, {}, folder_id);
        return true;
}

bool ReplSession::service_fleet_steer() {
        if (!mouse_fleet.pending) return false;
        const bool cancel = mouse_fleet.cancel;
        mouse_fleet.pending = false;
        mouse_fleet.cancel = false;
        if (cancel) return cancel_fleet_selection();
        return steer_fleet_selection();
}

}  // namespace arbiter
