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

std::string ReplSession::diff_apply_summary_for(const Pane& pane) const {
        std::string err;
        const std::string root =
            conversation_store.resolved_workspace_root(pane.conversation_id, &err);
        if (root.empty()) {
            return "Conversation workspace unavailable"
                   + (err.empty() ? std::string{} : (": " + err))
                   + ". Apply refused (will not use process cwd).";
        }
        return "Apply under " + root + ". Missing files are created.";
}

std::string ReplSession::apply_diff_proposal(Pane& pane, int id) {

        auto& store = pane.diff_proposals;
        auto prop = store.get(id);
        if (!prop) return "ERR: no patch #" + std::to_string(id);
        if (prop->status == arbiter::DiffProposalStatus::Applied) {
            return "ERR: patch #" + std::to_string(id) +
                   " already applied — /diff undo " + std::to_string(id) +
                   " to revert";
        }
        if (prop->status == arbiter::DiffProposalStatus::Rejected) {
            return "ERR: patch #" + std::to_string(id) + " was rejected";
        }
        std::string root_err;
        const std::string root =
            conversation_store.resolved_workspace_root(pane.conversation_id,
                                                       &root_err);
        if (root.empty()) {
            const std::string detail =
                root_err.empty() ? "conversation workspace unavailable"
                                 : root_err;
            store.mark_failed(id, detail);
            return "ERR: apply #" + std::to_string(id) + " failed: " + detail;
        }
        auto applied = arbiter::apply_unified_diff(prop->patch, root);
        if (!applied.ok) {
            store.mark_failed(id, applied.error);
            return "ERR: apply #" + std::to_string(id) +
                   " failed: " + applied.error;
        }
        arbiter::DiffUndoSnapshot snap;
        snap.resolved_path = applied.resolved_path;
        snap.had_file = applied.had_file;
        snap.pre_image = applied.pre_image;
        snap.post_image = applied.post_image;
        store.mark_applied(id, std::move(snap));
        std::string msg = "applied patch #" + std::to_string(id) +
            ": " + applied.path;
        if (!applied.had_file) msg += " (created)";
        msg += "\n  /diff undo " + std::to_string(id) +
            " to restore the previous contents";
        return msg;
}

std::vector<std::string> ReplSession::patch_preview_lines(const std::string& patch) {

        std::vector<std::string> out;
        std::istringstream ps(patch);
        std::string line;
        while (std::getline(ps, line) && out.size() < 6) {
            if (line.rfind("---", 0) == 0 ||
                line.rfind("+++", 0) == 0 ||
                line.rfind("@@", 0) == 0 ||
                line.rfind("diff ", 0) == 0 ||
                line.rfind("index ", 0) == 0) {
                continue;
            }
            if (line.size() > 72) line = line.substr(0, 69) + "...";
            out.push_back(std::move(line));
        }
        return out;
}

void ReplSession::handle_diff_decision(Pane& pane, int patch_id,
                                    InteractiveDecision d) {

        auto push = [&](const std::string& msg) {
            pane.output_queue.push_prose(
                {arbiter::styled_activity_line(msg, arbiter::StyleId::System)});
            pane.output_queue.end_message();
        };
        if (d == arbiter::InteractiveDecision::Allow ||
            d == arbiter::InteractiveDecision::AllowAll) {
            push(apply_diff_proposal(pane, patch_id));
            return;
        }
        if (d == arbiter::InteractiveDecision::Deny) {
            if (pane.diff_proposals.mark_rejected(patch_id)) {
                auto prop = pane.diff_proposals.get(patch_id);
                push("rejected patch #" + std::to_string(patch_id) +
                     (prop ? (": " + prop->path) : std::string{}));
            } else {
                push("ERR: could not reject patch #" + std::to_string(patch_id));
            }
        }
        // Cancel: leave pending.
}

bool ReplSession::service_interactive() {

        auto entry_opt = interactive_prompts.take_front();
        if (!entry_opt) return false;
        auto& entry = *entry_opt;
        auto& req = entry.request;

        Pane* target = nullptr;
        Pane* pane_ptr = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lk(layout_mu);
            if (req.pane) {
                target = static_cast<Pane*>(req.pane);
                // Verify the pane is still in the layout.
                bool alive = false;
                layout_ptr->for_each_pane([&](Pane& p) {
                    if (&p == target) alive = true;
                });
                if (!alive) target = nullptr;
            }
            pane_ptr = target ? target : &layout_ptr->focused();
        }
        // Main-thread only: pane lifetime is stable across the blocking
        // key read below (close/chord also run on this thread).
        Pane& pane = *pane_ptr;

        // Skip DiffReview cards whose proposal was already resolved (e.g.
        // auto-review + /diff review both queued the same id).
        if (req.kind == arbiter::InteractiveKind::DiffReview) {
            // Never use the focused pane's store for another pane's patch id.
            if (!target) {
                if (req.on_complete)
                    req.on_complete(arbiter::InteractiveDecision::Cancel);
                if (entry.promise) {
                    arbiter::complete_prompt_promise(
                        entry.promise, arbiter::InteractiveDecision::Cancel);
                }
                return true;
            }
            auto prop = pane.diff_proposals.get(req.patch_id);
            if (!prop) {
                if (req.on_complete)
                    req.on_complete(arbiter::InteractiveDecision::Cancel);
                if (entry.promise) {
                    arbiter::complete_prompt_promise(
                        entry.promise, arbiter::InteractiveDecision::Cancel);
                }
                return true;
            }
            if (prop->status == arbiter::DiffProposalStatus::Applied) {
                // Already applied — treat waiters as success, not cancel.
                if (entry.promise) {
                    arbiter::complete_prompt_promise(
                        entry.promise, arbiter::InteractiveDecision::Allow);
                }
                return true;
            }
            if (prop->status != arbiter::DiffProposalStatus::Pending &&
                prop->status != arbiter::DiffProposalStatus::Failed) {
                if (entry.promise) {
                    arbiter::complete_prompt_promise(
                        entry.promise, arbiter::InteractiveDecision::Cancel);
                }
                return true;
            }
        }

        arbiter::InteractiveDecision decision =
            arbiter::InteractiveDecision::Cancel;

        if (req.kind == arbiter::InteractiveKind::Confirm) {
            std::vector<std::string> preview = req.preview_lines;
            if (!req.summary.empty()) {
                preview.insert(preview.begin(), req.summary);
            }
            auto card = arbiter::styled_permission_card(
                req.action, req.target, preview);
            // Scroll mutations must hold layout_mu — the output pump draws
            // PaneScrollView under the same lock (UAF → DiffPanel::set_patch abort).
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                pane_history_push_prose(pane, card, true);
                present_holding_lock();
            }

            const int key = arbiter::read_confirm_key();
            if (key == 'y' || key == 'Y') {
                decision = arbiter::InteractiveDecision::Allow;
            } else if (key == 'A') {
                // Allow this confirm; also accept remaining file edits.
                decision = arbiter::InteractiveDecision::AllowAll;
            } else {
                decision = arbiter::InteractiveDecision::Deny;
            }
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                pane_history_push_prose(
                    pane,
                    {arbiter::styled_activity_line(
                        decision_is_affirmative(decision)
                            ? "[user accepted input]"
                            : "[user denied input]",
                        decision_is_affirmative(decision)
                            ? StyleId::Success
                            : StyleId::Error)},
                    true);
                present_holding_lock();
            }
        } else if (entry.enqueued_under_accept_edits) {
            // Session accept-edits was already on when this entry was queued.
            decision = arbiter::InteractiveDecision::Allow;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                pane_history_push_prose(
                    pane,
                    {arbiter::styled_activity_line(
                        "[diff auto-applied — accept edits on]",
                        StyleId::Success)},
                    true);
                present_holding_lock();
            }
        } else {
            // DiffReview card
            auto card = arbiter::styled_diff_review_card(
                req.patch_id, req.path, req.summary, req.preview_lines);
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                pane_history_push_prose(pane, card, true);
                present_holding_lock();
            }

            while (true) {
                char csi = 0;
                std::string csi_params;
                const int key = arbiter::read_history_sidebar_key(csi, csi_params);
                if (key < 0) {
                    decision = arbiter::InteractiveDecision::Cancel;
                    break;
                }
                if (key == 0x1B && (csi == 'M' || csi == 'm')
                    && !csi_params.empty() && csi_params[0] == '<') {
                    continue;
                }
                if (key == 'a') {
                    decision = arbiter::InteractiveDecision::Allow;
                    break;
                }
                if (key == 'A') {
                    decision = arbiter::InteractiveDecision::AllowAll;
                    break;
                }
                if (key == 'r' || key == 'R') {
                    decision = arbiter::InteractiveDecision::Deny;
                    break;
                }
                if (key == 0x1B && csi == 0) {
                    decision = arbiter::InteractiveDecision::Cancel;
                    break;
                }
            }

            const char* label =
                (decision == arbiter::InteractiveDecision::Allow)
                    ? "[diff apply]"
                : (decision == arbiter::InteractiveDecision::AllowAll)
                    ? "[diff allow all]"
                : (decision == arbiter::InteractiveDecision::Deny)
                    ? "[diff reject]"
                    : "[diff review cancelled]";
            const StyleId style =
                (decision == arbiter::InteractiveDecision::Allow ||
                 decision == arbiter::InteractiveDecision::AllowAll)
                    ? StyleId::Success
                : (decision == arbiter::InteractiveDecision::Deny)
                    ? StyleId::Warning
                    : StyleId::Dim;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                pane_history_push_prose(
                    pane,
                    {arbiter::styled_activity_line(label, style)},
                    true);
                present_holding_lock();
            }
        }

        if (decision == arbiter::InteractiveDecision::AllowAll) {
            if (req.kind == arbiter::InteractiveKind::DiffReview) {
                // Apply the current patch first, then remaining queued diffs,
                // so multi-file / same-file FIFO order is preserved.
                if (req.on_complete && (req.auto_review || !entry.promise)) {
                    req.on_complete(decision);
                } else if (target) {
                    // Blocking /diff review: apply on main before remaining.
                    handle_diff_decision(*target, req.patch_id, decision);
                }
                interactive_prompts.allow_remaining_diff_reviews();
                if (entry.promise) {
                    arbiter::complete_prompt_promise(entry.promise, decision);
                }
                return true;
            }
            // Confirm `A`: allow this exec and auto-accept future file
            // diffs — do not silently apply already-queued patches.
            interactive_prompts.set_accept_edits(true);
        }

        // Auto-review (and any request with on_complete) applies here.
        // Blocking /diff review waiters apply in their own handler after
        // the promise resolves — skip on_complete for those to avoid
        // double-apply.  auto_review entries always use on_complete.
        if (req.on_complete && (req.auto_review || !entry.promise)) {
            req.on_complete(decision);
        }
        if (entry.promise) {
            arbiter::complete_prompt_promise(entry.promise, decision);
        }
        return true;
}

}  // namespace arbiter
