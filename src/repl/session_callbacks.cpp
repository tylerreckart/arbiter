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

std::unique_ptr<Pane> ReplSession::make_pane() {

        auto p = std::make_unique<Pane>();
        // Wire pump wakeup so any output push wakes the drain thread
        // immediately rather than waiting for the next 30ms tick.
        p->output_queue.set_notify_fn([this](){
            if (pump_notify) pump_notify();
        });
        p->current_agent = "index";
        p->current_model = orch.get_agent_model(p->current_agent);
        // New splits inherit the focused pane's conversation (same buffer in
        // a new window).  The very first pane binds to the store's active id.
        if (layout_ptr) {
            p->conversation_id = layout_ptr->focused().conversation_id;
        } else {
            p->conversation_id = conversation_active_id();
        }
        pane_history_init(*p);

        p->editor.set_shared_history(shared_history);
        p->editor.set_max_history(1000);
        p->editor.set_palette_items(palette_items);
        p->editor.set_present_fn([this]() { if (pump_notify) pump_notify(); });

        p->editor.set_completion_provider(
            [this](const std::string& buf, const std::string& tok)
            -> std::vector<std::string> {
                auto match = [&](const std::vector<std::string>& candidates) {
                    std::vector<std::string> out;
                    for (auto& c : candidates)
                        if (c.substr(0, tok.size()) == tok) out.push_back(c);
                    return out;
                };
                std::string cmd;
                {
                    std::istringstream iss(buf);
                    iss >> cmd;
                    if (!cmd.empty() && cmd[0] == '/') cmd = cmd.substr(1);
                }
                bool only_cmd = (buf.find(' ') == std::string::npos);
                if (only_cmd || buf.empty()) {
                    return match({"/send","/ask","/use","/agents","/status","/tokens",
                                  "/create","/remove","/reset","/compact","/model",
                                  "/pane","/find",
                                  "/loop","/loops","/log","/watch",
                                  "/kill","/suspend","/resume","/inject",
                                  "/fetch","/mem","/search","/browse",
                                  "/todo","/schedule","/exec","/diff","/write",
                                  "/read","/list","/map","/mcp","/a2a","/lesson",
                                  "/plan","/theme","/verbose","/chat","/quit","/help"});
                }
                if (cmd == "diff") {
                    return match({"review","list","apply","reject","undo"});
                }
                if (cmd == "send" || cmd == "use" || cmd == "loop" || cmd == "model" ||
                    cmd == "reset" || cmd == "compact" || cmd == "pane") {
                    auto agents = orch.list_agents();
                    agents.push_back("index");
                    return match(agents);
                }
                if (cmd == "kill"    || cmd == "suspend" || cmd == "resume" ||
                    cmd == "watch"   || cmd == "log"     || cmd == "inject") {
                    return match(loops.list_ids());
                }
                if (cmd == "mem") return match({"write","read","show","clear","shared",
                                                "search","entries","entry","add"});
                if (cmd == "todo") return match({"list","add","start","done","cancel","reorder"});
                if (cmd == "schedule") return match({"list","cancel","pause","resume"});
                if (cmd == "chat") return match({"list", "new", "switch", "search", "title", "delete", "purge"});
                if (cmd == "mcp") return match({"tools","call"});
                if (cmd == "a2a") return match({"list","call"});
                if (cmd == "theme") return match(arbiter::tui_list_available_themes(dir));
                return {};
            });

        Pane* raw = p.get();
        p->editor.set_scroll_handler([this, raw](int direction, int step) {
            // Must serialize against the output pump's drain/draw under
            // layout_mu — replay_load_previous_chunk mutates segments_.
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            const int max_off = pane_history_max_scroll(*raw);
            if (direction < 0) {
                raw->scroll_offset = std::min(raw->scroll_offset + step, max_off);
                // Hit the top of currently-loaded scrollback: pull in the
                // next chunk of older transcript history, if any is behind
                // the gap marker (see replay_transcript/kReplayTailMessages).
                if (raw->scroll_offset >= max_off && raw->scroll && raw->scroll->has_gap()) {
                    ConversationScope scope(raw->conversation_id);
                    const std::string& agent = raw->current_agent.empty()
                        ? "index" : raw->current_agent;
                    const auto history = orch.get_agent_history(agent);
                    arbiter::replay_load_previous_chunk(*raw, history);
                }
            } else {
                raw->scroll_offset = std::max(0, raw->scroll_offset - step);
                raw->new_while_scrolled = 0;
                if (raw->scroll_offset == 0) raw->tui.clear_status();
            }
            if (pump_notify) pump_notify();
        });
        p->editor.set_code_expand_handler([this, raw]() {
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            if (pane_history_toggle_code_block(*raw, raw->scroll_offset) && pump_notify) {
                pump_notify();
            }
        });
        p->editor.set_cancel_handler(
            [this, raw]() {
            // Esc clears an active scrollback selection first; a second Esc
            // (or Esc with no selection) cancels the in-flight turn.
            if (pane_history_has_selection(*raw)) {
                pane_history_clear_selection(*raw);
                if (pump_notify) pump_notify();
                return;
            }
            // Esc during a deferred switch/delete wait abandons the pending
            // op without a second global cancel (token cancel already fired).
            if (pending_after_cancel.kind != PendingAfterCancel::Kind::None) {
                const char* status = pending_after_cancel.abandon_status
                    ? pending_after_cancel.abandon_status
                    : "Cancelled";
                pending_after_cancel = {};
                pending_cancel_wait.store(false);
                raw->thinking.stop();
                raw->tui.set_status(status);
                if (pump_notify) pump_notify();
                return;
            }
            // Unblock any in-flight confirm / diff-review waiter before
            // cancelling the turn — otherwise fut.get() hangs and pane
            // close blocks forever in join.
            fail_pending_prompts();
            // Scoped cancel: stop this pane's turn only so sibling panes
            // keep streaming (#46 / #48).  Prefer the pane token; fall back
            // to process-wide cancel only for Esc on the focused pane when
            // no token is installed (legacy / race), never from idle-pane
            // close/delete helpers.
            auto token = std::atomic_load(&raw->turn_cancel);
            if (is_remote() || token) {
                cancel_pane_turn(*raw);
            } else {
                orch.cancel();
                // Idle Esc (no turn token): cancel() still arms hard_cancel
                // for kill-switch semantics, but nothing is in flight — clear
                // so the next intentional user turn is not rejected as
                // cancelled.  If the queue is busy we may be in the race
                // before the token is installed; leave hard_cancel armed.
                if (!raw->cmd_queue.is_busy()) {
                    orch.clear_sticky_cancel();
                }
            }
            raw->multiline_accum.clear();
            raw->output_queue.push_prose(
                {arbiter::styled_activity_line("[interrupted]", StyleId::Error)});
            raw->output_queue.end_message();
        });
        // Chord prefix: Ctrl-w.  Recognized follow-ups: w (next pane),
        // h (horizontal split), v (vertical split), s (sidebar toggle),
        // c (close pane), z (zoom), t/b (history sidebar); Ctrl-w itself
        // is a synonym for 'w'.
        p->editor.set_chord_handler([](char cmd) -> bool {
            return cmd == 'w' || cmd == 'h' || cmd == 's' || cmd == 'v' || cmd == 'c'
                || cmd == 'z' || cmd == 't' || cmd == 'b'
                || cmd == 0x17;
        });
        p->editor.set_mouse_handler([this](const opentui::MouseEvent& ev) {
            return route_mouse(ev);
        });
        return p;
}

void ReplSession::install_orch_callbacks() {
    // Provider reasoning/thinking → collapsible ThinkingSegment (when emitted).
    // Header ThinkingIndicator remains the wait-state spinner for all models.
    // When an assistant message already exists (tool / nested phase), also
    // persist onto the pane agent so conversation switch rebuilds the rows.
    orch.client().set_reasoning_callback([&](const std::string& delta) {
        Pane* p = g_active_pane;
        if (!p) return;
        p->output_queue.push_thinking(delta, p->current_agent);
        orch.append_thinking(p->current_agent, delta);
    });

    // ── Orchestrator callbacks ─────────────────────────────────────────────
    // All pane-facing callbacks route through g_active_pane (thread-local),
    // which each pane's exec thread sets at startup. /parallel workers pin
    // the same pane via worker_pane_binder.
    orch.set_progress_callback([&](const std::string& agent_id,
                                    const std::string& content) {
        Pane* p = g_active_pane;
        if (!p) return;
        // One header per distinct sub-agent per master turn (not every API
        // iteration — that was flooding scroll with repeated → agent rows).
        if (p->last_interim_agent != agent_id) {
            p->output_queue.push_prose({arbiter::styled_interim_header(agent_id)});
            p->last_interim_agent = agent_id;
        }
        arbiter::StreamRenderer renderer(arbiter::kInterim, p->output_queue);
        renderer.feed(content);
        renderer.flush();
    });
    orch.set_tool_status_callback([&](const arbiter::ToolActivityEvent& ev) {
        Pane* p = g_active_pane;
        if (p) {
            // In-scroll timeline row (Started creates, Finished updates).
            p->output_queue.push_tool(ev);
            // Do NOT call begin() here — turn entry already arms the spinner.
            // begin() zeroes counters, so N tools would always display as "1".
            if (ev.phase == arbiter::ToolActivityEvent::Phase::Finished) {
                p->tool_indicator.bump(ev.label, ev.ok);
            }
        }
        if (ev.phase == arbiter::ToolActivityEvent::Phase::Finished) {
            sidebar.record_tool(ev.label, ev.ok);
            // Persist for conversation-switch replay.  Pane history is what
            // apply_conversation_to_pane rebuilds (usually "index"), so the
            // pane agent always gets the row.  When a nested /agent dispatched
            // the tool, also mirror onto that child so its own history stays
            // accurate if inspected later.
            if (p) {
                arbiter::ToolTraceEntry te;
                te.id = ev.id;
                te.label = ev.label;
                te.kind = ev.kind;
                te.detail = ev.detail;
                te.ok = ev.ok;
                te.result_preview = ev.result_preview;
                orch.append_tool_trace(p->current_agent, te);
                if (!ev.agent_id.empty() && ev.agent_id != p->current_agent) {
                    orch.append_tool_trace(ev.agent_id, std::move(te));
                }
            }
        }
    });
    orch.set_cost_callback([&](const std::string& agent_id,
                                 const std::string& model,
                                 const arbiter::ApiResponse& resp) {
        sidebar.record_turn(agent_id, model, resp);
        // Prefer the ConversationScope key — cost_cb runs on the pane exec
        // thread inside handle_line()'s scope, even when g_active_pane is unset
        // (e.g. nested /parallel workers that re-pin late).
        const std::string& cid = arbiter::agent_conversation_key();
        const int delta = resp.input_tokens + resp.output_tokens;
        if (delta > 0 && !cid.empty()) {
            conversation_store.add_tokens(cid, delta);
        }
    });
    orch.set_agent_start_callback([&](const std::string& /*agent_id*/) {
        Pane* p = g_active_pane;
        if (p) p->thinking.start();
    });
    // Mid-turn durability: after each committed model iteration and each
    // tool-result envelope, queue an autosave so quit/cancel/SIGKILL cannot
    // drop completed tool work that is already in histories_.
    orch.set_history_checkpoint_callback([&]() {
        const std::string& cid = arbiter::agent_conversation_key();
        if (cid.empty()) return;
        conversation_store.save_async(cid, orch);
    });
    orch.set_escalation_callback([&](const std::string& agent_id,
                                      int /*stream_id*/,
                                      const std::string& reason) {
        Pane* p = g_active_pane;
        if (!p) return;
        std::string text = "[advisor halt: " + agent_id + "] " + reason;
        p->output_queue.push_prose(
            {arbiter::styled_activity_line(std::move(text), arbiter::StyleId::Error)});
        p->output_queue.end_message();
    });

    orch.set_advisor_event_callback([&](const arbiter::Orchestrator::AdvisorEvent& ev) {
        Pane* p = g_active_pane;
        if (!p) return;
        if (ev.kind == "gate_continue") return;  // quiet success
        arbiter::StyleId style = arbiter::StyleId::System;
        std::string label;
        if      (ev.kind == "consult")       { label = "advisor consult"; style = arbiter::StyleId::System; }
        else if (ev.kind == "gate_redirect") { label = "advisor redirect"; style = arbiter::StyleId::Warning; }
        else if (ev.kind == "gate_halt")     { label = "advisor halt";    style = arbiter::StyleId::Error;  }
        else if (ev.kind == "gate_budget")   { label = "advisor budget";  style = arbiter::StyleId::Error;  }
        else                                  { label = ev.kind;            style = arbiter::StyleId::System; }
        std::string detail = ev.detail;
        if (detail.size() > 200) { detail.resize(197); detail += "..."; }
        std::string text = "[" + label + ": " + ev.agent_id + "]";
        if (!detail.empty()) text += " " + detail;
        p->output_queue.push_prose(
            {arbiter::styled_activity_line(std::move(text), style)});
        p->output_queue.end_message();
    });

    orch.set_confirm_callback([&](const arbiter::ConfirmRequest& req) -> bool {
        return interactive_prompts.request_confirm(req);
    });

    // Auto-enqueue interactive diff review when ```diff fences register.
    arbiter::pane_history_set_diff_auto_review(
        [&](Pane& pane, const arbiter::DiffProposal& prop) {
            Pane* pane_ptr = &pane;
            const int id = prop.id;
            arbiter::InteractiveRequest req;
            req.kind = arbiter::InteractiveKind::DiffReview;
            req.action = "diff";
            req.target = prop.path;
            req.patch_id = id;
            req.path = prop.path;
            req.summary = diff_apply_summary_for(pane);
            req.preview_lines = patch_preview_lines(prop.patch);
            req.pane = pane_ptr;
            req.auto_review = true;
            req.on_complete = [this, pane_ptr, id](InteractiveDecision d) {
                if (!layout_ptr || !pane_ptr) return;
                bool alive = false;
                layout_ptr->for_each_pane([&](Pane& p) {
                    if (&p == pane_ptr) alive = true;
                });
                if (!alive) return;
                handle_diff_decision(*pane_ptr, id, d);
            };
            interactive_prompts.enqueue_auto(std::move(req));
        });

}

std::string ReplSession::spawn_pane(const std::string& req_agent,
                                   const std::string& message) {

        if (req_agent != "index" && !orch.has_agent(req_agent))
            return "ERR: no agent '" + req_agent + "'";

        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        clear_mouse_drag();

        if (layout_ptr->pane_count() >= kMaxLayoutSnapshotLeaves) {
            return "ERR: pane cap reached (" +
                   std::to_string(kMaxLayoutSnapshotLeaves) +
                   " open); close one before spawning more";
        }

        Pane* spawner_pane = g_active_pane;
        // Mid-close: main has moved this pane's exec_thread out for join, so
        // the Pane is still in the tree but must not become a parent — that
        // would leave children with a dangling parent_pane after destroy.
        if (spawner_pane && !spawner_pane->exec_thread.joinable()) {
            spawner_pane = nullptr;
        } else if (spawner_pane) {
            bool spawner_alive = false;
            layout_ptr->for_each_pane([&](Pane& p) {
                if (&p == spawner_pane) spawner_alive = true;
            });
            if (!spawner_alive) spawner_pane = nullptr;
        }
        std::string captured_agent = req_agent;
        Pane* new_pane_ptr = layout_ptr->split_focused(
            LayoutTree::Orient::Vertical,
            [this, captured_agent]() -> std::unique_ptr<Pane> {
                auto p = make_pane();
                if (!p) return p;
                p->current_agent = captured_agent;
                p->current_model = orch.get_agent_model(captured_agent);
                pane_history_clear(*p);
                return p;
            },
            /*focus_new=*/false);

        if (!new_pane_ptr) {
            return "ERR: focused pane too small to split";
        }

        Pane& new_pane = *new_pane_ptr;
        new_pane.parent_pane   = spawner_pane;
        new_pane.spawn_message = message;
        new_pane.spawn_flowed.store(false);
        start_pane_thread(new_pane);
        new_pane.cmd_queue.push(message);

        sync_layout_to_terminal();
        layout_ptr->for_each_pane([&](Pane& p) {
            pane_history_set_cols(p, p.tui.cols());
        });
        persist_layout();
        present_holding_lock();

        refresh_focused_input.store(true);
        layout_ptr->focused().editor.interrupt();

        return "OK: spawned pane on agent '" + captured_agent +
               "'; output streams in its own view";
}

}  // namespace arbiter
