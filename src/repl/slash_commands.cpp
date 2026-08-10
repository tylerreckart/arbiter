#include "repl/session.h"
#include "repl/session_internal.h"
#include "cli.h"
#include "cli_helpers.h"
#include "orchestrator.h"
#include "agent_conversation.h"
#include "commands.h"
#include "constitution.h"
#include "markdown.h"
#include "model_catalog.h"
#include "model_context.h"
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

void ReplSession::handle_line(Pane& pane, const std::string& line) {

        auto& tui             = pane.tui;
        auto& output_queue    = pane.output_queue;
        auto& thinking        = pane.thinking;
        auto& tool_indicator  = pane.tool_indicator;
        auto& current_agent   = pane.current_agent;
        auto& current_model   = pane.current_model;

        auto push_sys = [&](const std::string& s) {
            output_queue.push_prose_msg(s, StyleId::System);
        };
        auto push_err = [&](const std::string& s) {
            output_queue.push_prose_msg(s, StyleId::Error);
        };
        auto push_status = [&](const std::string& s) {
            if (s.rfind("ERR:", 0) == 0) push_err(s);
            else push_sys(s);
        };
        // Loop logs embed render_markdown() ANSI — must stay on the TextSegment
        // path (push_msg). push_prose_msg would paint CSI escapes as glyphs.
        auto push_ansi = [&](const std::string& s) {
            output_queue.push_msg(s);
        };
        auto push_md = [&](const std::string& md) {
            MarkdownRenderer renderer;
            auto lines = renderer.feed_styled(md);
            auto tail = renderer.flush_styled();
            lines.insert(lines.end(), tail.begin(), tail.end());
            if (!lines.empty()) {
                output_queue.push_prose(lines);
                output_queue.end_message();
            }
        };

        if (line[0] == '/') {
            // Parse command
            std::istringstream iss(line.substr(1));
            std::string cmd;
            iss >> cmd;

            if (cmd == "quit" || cmd == "exit" || cmd == "q") {
                quit_requested = true; return;
            }

            if (cmd == "agents") {
                if (is_remote()) {
                    std::string err;
                    auto agents = remote->list_agents(&err);
                    if (agents.empty() && !err.empty()) {
                        push_status("ERR: " + err);
                        return;
                    }
                    std::string out;
                    if (agents.empty()) out = "  (no agents)\n";
                    else {
                        for (auto& a : agents) {
                            out += "  " + a.id;
                            if (!a.model.empty()) out += "  (" + a.model + ")";
                            out += "\n";
                        }
                    }
                    out += "\n";
                    push_sys(out);
                    return;
                }
                std::string out;
                for (auto& id : orch.list_agents()) out += "  " + id + "\n";
                out += "\n";
                push_sys(out);
                return;
            }
            if (cmd == "status") {
                if (is_remote()) {
                    std::ostringstream os;
                    os << "remote: " << remote->config().display_host << "\n"
                       << "  url:    " << remote->config().base_url << "\n";
                    if (!remote_tenant_name.empty())
                        os << "  tenant: " << remote_tenant_name << "\n";
                    os << "  auth:   bearer token\n"
                       << "  conv:   " << pane.conversation_id << "\n";
                    push_status(os.str());
                    return;
                }
                push_status(orch.global_status());
                return;
            }
            if (cmd == "find") {
                std::string rest;
                std::getline(iss, rest);
                size_t a = 0;
                while (a < rest.size() && std::isspace(static_cast<unsigned char>(rest[a]))) ++a;
                rest = rest.substr(a);
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
                    rest.pop_back();

                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                arbiter::PaneFindResult r;
                if (rest == "next" || (rest.empty() && !pane.find_term.empty())) {
                    r = pane_history_find_step(pane, +1);
                } else if (rest == "prev") {
                    r = pane_history_find_step(pane, -1);
                } else if (rest.empty()) {
                    push_status("Usage: /find <text>, then /find next|prev to cycle");
                    return;
                } else {
                    r = pane_history_find(pane, rest);
                }
                if (r.total == 0) {
                    tui.set_status("find \"" + pane.find_term + "\": no matches");
                } else {
                    // Include the absolute visual row so consecutive /find
                    // next paints differ by more than a single digit.  OpenTUI
                    // cell-diff otherwise often emits nothing when only the
                    // hit index changes (N/N → 1/N), which made PTY CI flake.
                    std::string status = "find \"" + pane.find_term + "\": "
                        + std::to_string(r.hit) + "/" + std::to_string(r.total);
                    if (r.row >= 0) {
                        status += " @";
                        status += std::to_string(r.row);
                    }
                    status += "  /find next|prev";
                    // Clear then set forces a full status-bar rewrite instead of
                    // a one-cell digit morph that OpenTUI may silently drop.
                    tui.clear_status();
                    if (ui_ctx.present_all) ui_ctx.present_all();
                    ot_session.flush_display();
                    tui.set_status(status);
                }
                // Back-to-back /find and /find next calls are the only case
                // in the app where the *entire* visible delta is a one-line
                // status-bar change with no accompanying scrollback content
                // change (jump_to_row can also leave scroll_offset
                // unchanged, e.g. cycling between two matches that map to
                // the same visual row).  OpenTUI's diffed render occasionally
                // fails to pick up such a narrow change from the normal
                // pump-driven cadence — same class of issue /theme already
                // works around (via refresh_chrome()) by forcing a full
                // repaint instead of relying on pump_notify()'s diffed redraw.
                if (ui_ctx.present_all) ui_ctx.present_all();
                ot_session.flush_display();
                return;
            }
            if (cmd == "tokens") {
                push_status(sidebar.tokens_report());
                return;
            }
            if (cmd == "use" || cmd == "switch") {
                std::string id;
                iss >> id;
                if (is_remote()) {
                    if (id.empty()) {
                        push_status("Usage: /use <agent>");
                        return;
                    }
                    bool found = (id == "index");
                    if (!found) {
                        std::string err;
                        for (const auto& a : remote->list_agents(&err)) {
                            if (a.id == id) { found = true; break; }
                        }
                    }
                    if (!found) {
                        push_status("ERR: no remote agent '" + id + "'");
                        return;
                    }
                    current_agent = id;
                    current_model = "remote";
                    pane.original_task.clear();
                    push_status(
                        "agent: " + id +
                        " (remote conversations pin agent at create; "
                        "follow-ups reuse the conversation snapshot)");
                    return;
                }
                if (id == "index" || orch.has_agent(id)) {
                    current_agent = id;
                    current_model = orch.get_agent_model(id);
                    pane.original_task.clear();
                } else {
                    push_status("ERR: no agent '" + id + "'");
                }
                return;
            }
            if (cmd == "send") {
                std::string id;
                iss >> id;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                reveal_sidebar();
                try {
                    if (!cfg.verbose) tool_indicator.begin();
                    pane.last_interim_agent.clear();
                    if (is_remote()) {
                        // Remote conversations pin agent at create time;
                        // /send still posts to the bound conversation.
                        (void)id;
                        auto resp = run_remote_turn(pane, msg);
                        tool_indicator.finalize();
                        output_queue.end_message();
                        if (!resp.ok) {
                            output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                        }
                    } else {
                        arbiter::StreamRenderer renderer(master_stream_policy(cfg), output_queue);
                        auto resp = orch.send_streaming(id, msg, [&](const std::string& chunk) {
                            renderer.feed(chunk);
                        });
                        renderer.flush();
                        // Per-tool rows already landed via ToolActivityEvent; just
                        // clear the mid-separator spinner.
                        tool_indicator.finalize();
                        // Separator: md.flush() guarantees the stream ends with
                        // a `\n`, so one more gives exactly one blank line before
                        // the next message.
                        output_queue.end_message();
                        if (!resp.ok) {
                            output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                        }
                    }
                } catch (const std::exception& e) {
                    output_queue.push_prose_msg("ERR: " + std::string(e.what()), StyleId::Error);
                }
                thinking.stop();
                return;
            }
            if (cmd == "pane") {
                // Manual /pane <agent> <msg> from the REPL — delegates to
                // the same helper the orchestrator uses when an agent emits
                // /pane in its response.  Fire-and-queue: the new pane runs
                // the message on its own exec thread, result flows back to
                // the pane that issued /pane when the task completes.
                std::string id;
                iss >> id;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                if (id.empty() || msg.empty()) {
                    push_status(
                        "Usage: /pane <agent-id> <message>");
                    return;
                }
                std::string result = spawn_pane(id, msg);
                push_status(result);
                return;
            }
            if (cmd == "ask") {
                std::string query;
                std::getline(iss, query);
                if (!query.empty() && query[0] == ' ') query.erase(0, 1);
                reveal_sidebar();
                try {
                    if (!cfg.verbose) tool_indicator.begin();
                    pane.last_interim_agent.clear();
                    if (is_remote()) {
                        auto resp = run_remote_turn(pane, query);
                        tool_indicator.finalize();
                        output_queue.end_message();
                        if (!resp.ok) {
                            output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                        }
                    } else {
                        arbiter::StreamRenderer renderer(master_stream_policy(cfg), output_queue);
                        auto resp = orch.send_streaming("index", query, [&](const std::string& chunk) {
                            renderer.feed(chunk);
                        });
                        renderer.flush();
                        tool_indicator.finalize();
                        output_queue.end_message();
                        if (!resp.ok) {
                            output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                        }
                    }
                } catch (const std::exception& e) {
                    output_queue.push_prose_msg("ERR: " + std::string(e.what()), StyleId::Error);
                }
                thinking.stop();
                return;
            }
            if (cmd == "create") {
                std::string id;
                iss >> id;
                try {
                    auto config = arbiter::master_constitution();
                    config.name = id;
                    orch.create_agent(id, std::move(config));
                    push_status("Created: " + id + " (default config)\n"
                                      "Edit ~/.arbiter/agents/" + id + ".json to customize");
                } catch (const std::exception& e) {
                    push_status("ERR: " + std::string(e.what()));
                }
                return;
            }
            if (cmd == "remove") {
                std::string id;
                iss >> id;
                orch.remove_agent(id);
                push_status("Removed: " + id);
                if (current_agent == id) current_agent = "index";
                return;
            }
            if (cmd == "reset") {
                std::string id;
                iss >> id;
                if (id.empty()) id = current_agent;
                try {
                    orch.get_agent(id).reset_history();
                    push_status("History cleared: " + id);
                    conversation_store.save_async(pane.conversation_id, orch);
                } catch (const std::exception& e) {
                    push_status("ERR: " + std::string(e.what()));
                }
                return;
            }
            if (cmd == "compact") {
                std::string id;
                iss >> id;
                if (id.empty()) id = current_agent;
                try {
                    auto& agent = orch.get_agent(id);
                    if (agent.force_compact()) {
                        auto note = agent.take_compaction_notice();
                        push_status(note.empty()
                            ? ("Compacted: " + id)
                            : note);
                        conversation_store.save_async(pane.conversation_id, orch);
                    } else {
                        push_status("Nothing to compact for " + id +
                                    " (history already within keep window, "
                                    "or summarize failed).");
                    }
                } catch (const std::exception& e) {
                    push_status("ERR: " + std::string(e.what()));
                }
                return;
            }
            if (cmd == "loop") {
                std::string id;
                iss >> id;
                std::string prompt;
                std::getline(iss, prompt);
                if (!prompt.empty() && prompt[0] == ' ') prompt.erase(0, 1);
                if (id.empty() || prompt.empty()) {
                    push_status("Usage: /loop <agent> <initial prompt>");
                    return;
                }
                if (id != "index" && !orch.has_agent(id)) {
                    push_status("ERR: no agent '" + id + "'");
                    return;
                }
                std::string lid = loops.start(orch, id, prompt, &output_queue);
                push_status("Loop started: " + lid + " (agent: " + id + ")");
                return;
            }
            if (cmd == "loops") {
                push_status(loops.list());
                return;
            }
            if (cmd == "kill") {
                std::string lid;
                iss >> lid;
                if (loops.kill(lid))
                    push_status("Killed: " + lid);
                else
                    push_status("ERR: no loop '" + lid + "'");
                return;
            }
            if (cmd == "suspend") {
                std::string lid;
                iss >> lid;
                if (loops.suspend(lid))
                    push_status("Suspended: " + lid);
                else
                    push_status("ERR: no loop '" + lid + "' or not running");
                return;
            }
            if (cmd == "resume") {
                std::string lid;
                iss >> lid;
                if (loops.resume(lid))
                    push_status("Resumed: " + lid);
                else
                    push_status("ERR: no loop '" + lid + "' or not suspended");
                return;
            }
            if (cmd == "inject") {
                std::string lid;
                iss >> lid;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                if (loops.inject(lid, msg))
                    push_status("Injected into " + lid);
                else
                    push_status("ERR: no loop '" + lid + "'");
                return;
            }
            if (cmd == "log") {
                std::string lid;
                iss >> lid;
                if (lid.empty()) {
                    push_status("Usage: /log <loop-id> [last-N]");
                    return;
                }
                int n = 0;
                iss >> n;
                push_ansi(loops.log(lid, n));
                return;
            }
            if (cmd == "watch") {
                std::string lid;
                iss >> lid;
                if (lid.empty()) {
                    push_status("Usage: /watch <loop-id>");
                    return;
                }
                if (loops.is_stopped(lid) && loops.log_count(lid) == 0) {
                    push_status("ERR: no loop '" + lid + "'");
                    return;
                }
                // Dump everything buffered so far
                size_t seen = loops.log_count(lid);
                output_queue.push(loops.log(lid, 0));
                if (!loops.is_stopped(lid)) {
                    push_sys("--- watching " + lid +
                             " — press Enter to detach ---");
                    // Tail new entries — exec thread polls while main thread flushes
                    while (!loops.is_stopped(lid)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        size_t now = loops.log_count(lid);
                        if (now > seen) {
                            output_queue.push(loops.log_since(lid, seen));
                            seen = now;
                        }
                    }
                    // Flush any remaining entries after stop
                    size_t now = loops.log_count(lid);
                    if (now > seen) {
                        output_queue.push(loops.log_since(lid, seen));
                    }
                    if (loops.is_stopped(lid)) {
                        push_sys("--- loop finished ---");
                    } else {
                        push_sys("--- detached ---");
                    }
                }
                return;
            }
            if (cmd == "fetch") {
                std::string url;
                iss >> url;
                if (url.empty()) {
                    push_status("Usage: /fetch <url>");
                    return;
                }
                thinking.start("fetching");
                std::string content = fetch_url(url);
                thinking.stop();
                if (content.substr(0, 4) == "ERR:") {
                    push_err(content);
                    return;
                }
                static constexpr size_t kFetchLimit = 32768;
                if (content.size() > kFetchLimit) {
                    content.resize(kFetchLimit);
                    content += "\n... [content truncated to 32 KB]";
                }
                std::string msg = "[FETCHED: " + url + "]\n" + content +
                                  "\n[END FETCHED]\n";
                try {
                    thinking.start("generating");
                    auto resp = orch.send(current_agent, msg);
                    thinking.stop();
                    if (resp.ok) {
                        push_md(resp.content);
                    } else {
                        output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                    }
                } catch (const std::exception& ex) {
                    thinking.stop();
                    push_err("ERR: " + std::string(ex.what()));
                }
                return;
            }
            if (cmd == "search") {
                // Operator /search mirrors /fetch: bypass the focused
                // agent's capability gate, run the wired Brave invoker,
                // and inject results into the conversation so the agent
                // can synthesize — not just flash status text.
                std::string rest;
                std::getline(iss, rest);
                if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t'))
                    rest.pop_back();
                std::string query = rest;
                int top_n = 10;
                {
                    auto pos = query.rfind(" top=");
                    if (pos != std::string::npos) {
                        try {
                            int parsed = std::stoi(query.substr(pos + 5));
                            if (parsed > 0) {
                                top_n = std::min(parsed, 20);
                                query.resize(pos);
                            }
                        } catch (...) { /* keep query, default top_n */ }
                    }
                }
                while (!query.empty() && (query.back() == ' ' || query.back() == '\t'))
                    query.pop_back();
                if (query.empty()) {
                    push_status("Usage: /search <query> [top=N]");
                    return;
                }
                thinking.start("searching");
                std::string body = orch.web_search(query, top_n);
                thinking.stop();
                if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0) {
                    push_err(body);
                    return;
                }
                static constexpr size_t kSearchLimit = 32768;
                if (body.size() > kSearchLimit) {
                    body.resize(kSearchLimit);
                    body += "\n... [truncated]";
                }
                std::string msg = "[/search " + query + "]\n" + body;
                if (msg.empty() || msg.back() != '\n') msg.push_back('\n');
                msg += "[END SEARCH]\n";
                try {
                    thinking.start("generating");
                    auto resp = orch.send(current_agent, msg);
                    thinking.stop();
                    if (resp.ok) {
                        push_md(resp.content);
                    } else {
                        output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                    }
                } catch (const std::exception& ex) {
                    thinking.stop();
                    push_err("ERR: " + std::string(ex.what()));
                }
                return;
            }
            if (cmd == "model") {
                std::string id, model;
                iss >> id >> model;
                if (id.empty()) {
                    std::string current_model;
                    try {
                        current_model = orch.get_agent(current_agent).config().model;
                    } catch (...) {}
                    auto items = menu_items_models(current_model);
                    if (items.empty()) {
                        push_status(format_model_catalog_list());
                        return;
                    }
                    overlay_menu.open(
                        MenuPurpose::Model,
                        "Models",
                        "\u2191\u2193 browse  Enter apply to " + current_agent
                            + "  Esc cancel",
                        std::move(items),
                        current_model,
                        current_agent,
                        /*max_width=*/72);
                    tui_begin_modal_dim();
                    refresh_chrome();
                    return;
                }
                if (model.empty()) {
                    try {
                        const auto& agent = orch.get_agent(id);
                        const std::string& cur = agent.config().model;
                        const int window = context_window_for_model(cur);
                        std::string msg = id + " model: " + cur
                            + "  ctx=" + format_context_window(window);
                        if (const auto* e = find_model_catalog_entry(cur)) {
                            msg += "  [";
                            msg += e->provider;
                            msg += "]";
                        } else {
                            msg += "  (not in catalogue — heuristic window)";
                        }
                        msg += "\n  /model                    — list catalogue\n"
                               "  /model " + id + " <model-id> — change model";
                        push_status(msg);
                    } catch (const std::exception& ex) {
                        push_status("ERR: " + std::string(ex.what()));
                    }
                    return;
                }
                try {
                    orch.get_agent(id).config_mut().model = model;
                    const int window = context_window_for_model(model);
                    std::string msg = id + " model -> " + model
                        + "  ctx=" + format_context_window(window);
                    if (!find_model_catalog_entry(model)) {
                        msg += "\n  note: id not in catalogue; routing still "
                               "works for known providers "
                               "(openrouter/…, ollama/…)";
                    }
                    push_status(msg);
                } catch (const std::exception& ex) {
                    push_status("ERR: " + std::string(ex.what()));
                }
                return;
            }
            if (cmd == "diff") {
                std::string sub;
                iss >> sub;
                auto& store = pane.diff_proposals;

                auto resolve_id = [&](bool prefer_pending) -> std::optional<int> {
                    std::string id_tok;
                    iss >> id_tok;
                    if (!id_tok.empty()) {
                        try {
                            return std::stoi(id_tok);
                        } catch (...) {
                            return std::nullopt;
                        }
                    }
                    if (prefer_pending) {
                        if (auto p = store.latest_pending()) return p->id;
                    } else {
                        if (auto p = store.latest_applied()) return p->id;
                    }
                    return std::nullopt;
                };

                auto apply_proposal = [&](int id) -> bool {
                    const std::string msg = apply_diff_proposal(pane, id);
                    push_status(msg);
                    return msg.rfind("ERR:", 0) != 0;
                };

                auto run_interactive_review = [&](std::optional<int> only_id) {
                    std::vector<int> ids;
                    if (only_id) {
                        auto prop = store.get(*only_id);
                        if (!prop) {
                            push_status("ERR: no patch #" +
                                        std::to_string(*only_id));
                            return;
                        }
                        if (prop->status != arbiter::DiffProposalStatus::Pending &&
                            prop->status != arbiter::DiffProposalStatus::Failed) {
                            push_status("ERR: patch #" + std::to_string(*only_id) +
                                        " is " +
                                        diff_proposal_status_label(prop->status) +
                                        " — only pending/failed can be reviewed");
                            return;
                        }
                        ids.push_back(*only_id);
                    } else {
                        for (const auto& p : store.list()) {
                            if (p.status == arbiter::DiffProposalStatus::Pending ||
                                p.status == arbiter::DiffProposalStatus::Failed) {
                                ids.push_back(p.id);
                            }
                        }
                    }
                    if (ids.empty()) {
                        push_status("No pending patches to review.\n"
                                    "  /diff list — all proposals; "
                                    "agent ```diff fences auto-prompt review");
                        return;
                    }
                    for (int id : ids) {
                        auto prop = store.get(id);
                        if (!prop) continue;
                        if (prop->status != arbiter::DiffProposalStatus::Pending &&
                            prop->status != arbiter::DiffProposalStatus::Failed) {
                            continue;
                        }
                        std::string summary = diff_apply_summary_for(pane);
                        if (!prop->error.empty()) {
                            summary = "Previously failed: " + prop->error;
                        }
                        const auto decision =
                            interactive_prompts.request_diff_review(
                                id, prop->path, summary,
                                patch_preview_lines(prop->patch), &pane);
                        if (decision == arbiter::InteractiveDecision::Allow ||
                            decision == arbiter::InteractiveDecision::AllowAll) {
                            // AllowAll may already have applied this id on the
                            // main thread — skip if no longer pending.
                            auto cur = store.get(id);
                            if (cur &&
                                (cur->status ==
                                     arbiter::DiffProposalStatus::Pending ||
                                 cur->status ==
                                     arbiter::DiffProposalStatus::Failed)) {
                                apply_proposal(id);
                            }
                            // AllowAll sets accept_edits; later ids short-circuit.
                        } else if (decision ==
                                   arbiter::InteractiveDecision::Deny) {
                            if (store.mark_rejected(id)) {
                                push_status("rejected patch #" +
                                            std::to_string(id) + ": " +
                                            prop->path);
                            } else {
                                push_status("ERR: could not reject patch #" +
                                            std::to_string(id));
                            }
                        } else {
                            push_status("review cancelled"
                                        " (remaining patches stay pending)");
                            return;
                        }
                    }
                };

                if (sub.empty() || sub == "review") {
                    // Optional N: review one patch.  Bare `/diff` / `/diff review`
                    // walks every pending/failed proposal.
                    std::string id_tok;
                    iss >> id_tok;
                    if (!id_tok.empty()) {
                        try {
                            run_interactive_review(std::stoi(id_tok));
                        } catch (...) {
                            push_status("Usage: /diff review [N]");
                        }
                    } else {
                        run_interactive_review(std::nullopt);
                    }
                    return;
                }

                if (sub == "list") {
                    auto items = store.list();
                    if (items.empty()) {
                        push_status("No diff proposals in this pane yet.\n"
                                    "  Agent ```diff fences register as Patch #N — "
                                    "then /diff or /diff apply N");
                        return;
                    }
                    std::ostringstream out;
                    out << "Diff proposals (this pane)\n";
                    for (const auto& p : items) {
                        out << "  #" << p.id << "  "
                            << diff_proposal_status_label(p.status)
                            << "  " << p.path;
                        if (!p.error.empty()) out << "  (" << p.error << ")";
                        out << "\n";
                    }
                    out << "\n  /diff [review] [N]   /diff apply [N]   "
                           "/diff reject [N]   /diff undo [N]";
                    push_status(out.str());
                    return;
                }

                if (sub == "reject") {
                    auto id = resolve_id(/*prefer_pending=*/true);
                    if (!id) {
                        push_status("Usage: /diff reject [N]\n"
                                    "  N defaults to the latest pending proposal.");
                        return;
                    }
                    auto prop = store.get(*id);
                    if (!prop) {
                        push_status("ERR: no patch #" + std::to_string(*id));
                        return;
                    }
                    if (!store.mark_rejected(*id)) {
                        push_status("ERR: patch #" + std::to_string(*id) +
                                    " is " + diff_proposal_status_label(prop->status) +
                                    " — only pending/failed can be rejected");
                        return;
                    }
                    push_status("rejected patch #" + std::to_string(*id) +
                                ": " + prop->path);
                    return;
                }

                if (sub == "apply") {
                    auto id = resolve_id(/*prefer_pending=*/true);
                    if (!id) {
                        push_status("Usage: /diff apply [N]\n"
                                    "  N defaults to the latest pending proposal.\n"
                                    "  Applies under this conversation's workspace "
                                    "directory (creates missing files; no write "
                                    "confirm); /diff undo N to revert.");
                        return;
                    }
                    apply_proposal(*id);
                    return;
                }

                if (sub == "undo") {
                    auto id = resolve_id(/*prefer_pending=*/false);
                    if (!id) {
                        push_status("Usage: /diff undo [N]\n"
                                    "  N defaults to the latest applied proposal.");
                        return;
                    }
                    auto prop = store.get(*id);
                    if (!prop) {
                        push_status("ERR: no patch #" + std::to_string(*id));
                        return;
                    }
                    if (prop->status != arbiter::DiffProposalStatus::Applied) {
                        push_status("ERR: patch #" + std::to_string(*id) +
                                    " is " + diff_proposal_status_label(prop->status) +
                                    " — nothing to undo");
                        return;
                    }
                    auto undone = arbiter::undo_unified_diff(prop->undo);
                    if (!undone.ok) {
                        push_status("ERR: undo #" + std::to_string(*id) +
                                    " failed: " + undone.error);
                        return;
                    }
                    store.clear_undo_after_revert(*id);
                    push_status("undid patch #" + std::to_string(*id) +
                                ": " + prop->path + " (pending again)");
                    return;
                }

                push_status("Usage: /diff [review] [N]|list|apply [N]|reject [N]|undo [N]\n"
                            "  /diff or /diff review — interactive [a]pply / [r]eject.\n"
                            "  Apply writes under this conversation's workspace directory;\n"
                            "  missing files are created (apply is the permission grant —\n"
                            "  no write confirm). Never falls back to process cwd.\n"
                            "  Omit N to target the latest pending (apply/reject) or applied (undo).");
                return;
            }
            if (cmd == "plan") {
                std::string subcmd;
                iss >> subcmd;
                if (subcmd != "execute") {
                    push_status("Usage: /plan execute <path>\n"
                                      "  Runs a plan file produced by /agent planner, executing each\n"
                                      "  phase sequentially and injecting prior outputs into dependents.");
                    return;
                }
                std::string path;
                iss >> path;
                if (path.empty()) {
                    push_status("Usage: /plan execute <path>");
                    return;
                }
                push_sys("[plan] executing: " + path + "]");
                auto result = orch.execute_plan(path,
                    [&](const std::string& msg) {
                        push_sys(msg);
                    });
                if (!result.ok) {
                    push_err("[plan] failed: " + result.error);
                } else {
                    push_sys("[plan] complete — " +
                             std::to_string(result.phases.size()) + " phase(s) executed]");
                    // Print final phase output (the deliverable)
                    if (!result.phases.empty()) {
                        auto& [num, name, out] = result.phases.back();
                        push_md(out);
                    }
                }
                return;
            }
            if (cmd == "help") {
                std::string topic;
                std::getline(iss, topic);
                if (!topic.empty() && topic[0] == ' ') topic.erase(0, 1);
                if (!topic.empty()) {
                    std::string result = orch.execute_slash_command(line, current_agent);
                    push_status(result.empty()
                        ? "Unknown help topic '" + topic + "'"
                        : result);
                    return;
                }
                auto items = menu_items_help();
                overlay_menu.open(
                    MenuPurpose::Help,
                    "Help",
                    "\u2191\u2193 browse  Enter detail  Esc close",
                    std::move(items),
                    /*select_id=*/{},
                    /*context=*/{},
                    /*max_width=*/64);
                tui_begin_modal_dim();
                refresh_chrome();
                return;
            }
            if (cmd == "chat") {
                std::string sub;
                iss >> sub;

                // Resolves "<n>" (1-based index into /chat list's order) or
                // an id-prefix to a real conversation id. Empty on no match.
                auto resolve_chat_target = [&](const std::string& arg) -> std::string {
                    if (arg.empty()) return {};
                    const auto entries = conversation_list();
                    const bool all_digits = std::all_of(arg.begin(), arg.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; });
                    if (all_digits) {
                        const unsigned long idx = std::stoul(arg);
                        if (idx >= 1 && idx <= entries.size()) return entries[idx - 1].id;
                        return {};
                    }
                    for (const auto& e : entries) {
                        if (e.id.rfind(arg, 0) == 0) return e.id;
                    }
                    return {};
                };

                if (sub == "list") {
                    if (is_remote()) remote_refresh_conversations();
                    const auto entries = conversation_list();
                    if (entries.empty()) {
                        push_status("(no conversations)");
                        return;
                    }
                    // Star the conversation bound to *this* pane, not the
                    // global active id — panes can show different threads.
                    const std::string starred = pane.conversation_id;
                    std::ostringstream out;
                    int n = 1;
                    for (const auto& e : entries) {
                        out << (e.id == starred ? "* " : "  ") << n << ". "
                            << (e.title.empty() ? "Untitled" : e.title)
                            << "  [" << e.id.substr(0, std::min<size_t>(8, e.id.size())) << "]\n";
                        ++n;
                    }
                    push_status(out.str());
                    return;
                }
                if (sub == "new") {
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({true, true, "", false, false});
                    }
                    if (layout_ptr) wake_main_input();
                    push_status("switching to a new conversation...");
                    return;
                }
                if (sub == "switch") {
                    std::string arg;
                    iss >> arg;
                    const std::string id = resolve_chat_target(arg);
                    if (id.empty()) {
                        push_status("Usage: /chat switch <n | id-prefix> (see /chat list)");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({true, false, id, false, false});
                    }
                    if (layout_ptr) wake_main_input();
                    push_status("switching...");
                    return;
                }
                if (sub == "title") {
                    std::string text;
                    std::getline(iss, text);
                    size_t a = 0;
                    while (a < text.size() && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
                    text = text.substr(a);
                    if (text.empty()) {
                        push_status("Usage: /chat title <text>");
                        return;
                    }
                    if (is_remote()) {
                        std::string err;
                        if (!remote->patch_conversation_title(pane.conversation_id, text, &err)) {
                            push_status("ERR: " + (err.empty() ? "rename failed" : err));
                            return;
                        }
                        remote_refresh_conversations();
                        refresh_history_sidebar_entries();
                        push_status("title: " + text);
                        return;
                    }
                    conversation_store.set_title_locked(pane.conversation_id, text);
                    push_status("title: " + text);
                    return;
                }
                if (sub == "delete") {
                    std::string arg;
                    iss >> arg;
                    const std::string id = resolve_chat_target(arg);
                    if (id.empty()) {
                        push_status("Usage: /chat delete <n | id-prefix> (see /chat list)");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({false, false, id, true, false});
                    }
                    if (layout_ptr) wake_main_input();
                    push_status("deleted (session file kept — /chat purge removes it)");
                    return;
                }
                if (sub == "purge") {
                    std::string arg;
                    iss >> arg;
                    const std::string id = resolve_chat_target(arg);
                    if (id.empty()) {
                        push_status("Usage: /chat purge <n | id-prefix> (see /chat list)");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({false, false, id, true, true});
                    }
                    if (layout_ptr) wake_main_input();
                    push_status("purged");
                    return;
                }
                if (sub == "search") {
                    std::string term;
                    std::getline(iss, term);
                    size_t a = 0;
                    while (a < term.size() && std::isspace(static_cast<unsigned char>(term[a]))) ++a;
                    term = term.substr(a);
                    if (term.empty()) {
                        push_status("Usage: /chat search <text>");
                        return;
                    }
                    if (is_remote()) {
                        // Remote full-text search is not on the wire yet —
                        // match against cached conversation titles / ids.
                        remote_refresh_conversations();
                        auto lower = [](std::string s) {
                            for (char& c : s) c = static_cast<char>(
                                std::tolower(static_cast<unsigned char>(c)));
                            return s;
                        };
                        const std::string needle = lower(term);
                        const auto entries = conversation_list();
                        const std::string starred = pane.conversation_id;
                        std::ostringstream out;
                        int n = 0;
                        for (const auto& e : entries) {
                            const std::string title = e.title.empty() ? "Untitled" : e.title;
                            if (lower(title).find(needle) == std::string::npos &&
                                lower(e.id).find(needle) == std::string::npos) {
                                continue;
                            }
                            ++n;
                            out << (e.id == starred ? "* " : "  ")
                                << title
                                << "  [" << e.id.substr(0, std::min<size_t>(8, e.id.size())) << "]\n";
                        }
                        if (n == 0) {
                            push_status("(no remote conversations match \"" + term + "\")");
                        } else {
                            out << "  Switch with /chat switch <id-prefix>.\n";
                            push_status(out.str());
                        }
                        return;
                    }
                    // Flush the coalesced autosave first so the active
                    // conversation's newest turns are searchable too.
                    conversation_store.flush();
                    const auto hits = conversation_store.search(term);
                    if (hits.empty()) {
                        push_status("(no conversations match \"" + term + "\")");
                        return;
                    }
                    const std::string starred = pane.conversation_id;
                    std::ostringstream out;
                    for (const auto& h : hits) {
                        out << (h.id == starred ? "* " : "  ")
                            << (h.title.empty() ? "Untitled" : h.title)
                            << "  [" << h.id.substr(0, std::min<size_t>(8, h.id.size())) << "]"
                            << "  (" << h.match_count
                            << (h.match_count == 1 ? " match)" : " matches)") << "\n";
                        if (!h.snippet.empty()) out << "      " << h.snippet << "\n";
                    }
                    out << "  Switch with /chat switch <id-prefix>.\n";
                    push_status(out.str());
                    return;
                }
                if (sub == "folder") {
                    if (is_remote()) {
                        push_status(
                            "ERR: /chat folder is not available in remote "
                            "(--connect) mode");
                        return;
                    }
                    std::string fsub;
                    iss >> fsub;

                    auto resolve_folder = [&](const std::string& arg) -> std::string {
                        if (arg.empty()) return {};
                        const auto folders = conversation_store.list_folders();
                        const bool all_digits = std::all_of(arg.begin(), arg.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; });
                        if (all_digits) {
                            for (const auto& f : folders) {
                                if (f.id == arg) return f.id;
                            }
                            // Also allow 1-based index into folder list.
                            try {
                                const unsigned long idx = std::stoul(arg);
                                if (idx >= 1 && idx <= folders.size()) {
                                    return folders[idx - 1].id;
                                }
                            } catch (...) {}
                            return {};
                        }
                        for (const auto& f : folders) {
                            if (f.name == arg) return f.id;
                            if (f.id.rfind(arg, 0) == 0) return f.id;
                        }
                        // Case-insensitive name match.
                        const std::string arg_lc = [&]{
                            std::string s = arg;
                            for (char& c : s) {
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            }
                            return s;
                        }();
                        for (const auto& f : folders) {
                            std::string n = f.name;
                            for (char& c : n) {
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            }
                            if (n == arg_lc) return f.id;
                        }
                        return {};
                    };

                    if (fsub == "list") {
                        const auto folders = conversation_store.list_folders();
                        if (folders.empty()) {
                            push_status("(no folders)");
                            return;
                        }
                        std::ostringstream out;
                        int n = 1;
                        for (const auto& f : folders) {
                            out << "  " << n << ". " << f.name
                                << "  [" << f.id << "]\n";
                            ++n;
                        }
                        push_status(out.str());
                        return;
                    }
                    if (fsub == "new") {
                        std::string name;
                        std::getline(iss, name);
                        size_t a = 0;
                        while (a < name.size() && std::isspace(static_cast<unsigned char>(name[a]))) ++a;
                        name = name.substr(a);
                        if (name.empty()) {
                            push_status("Usage: /chat folder new <name>");
                            return;
                        }
                        const std::string id = conversation_store.create_folder(name);
                        push_status("folder created: " + name + " [" + id + "]");
                        return;
                    }
                    if (fsub == "rename") {
                        std::string target;
                        iss >> target;
                        std::string name;
                        std::getline(iss, name);
                        size_t a = 0;
                        while (a < name.size() && std::isspace(static_cast<unsigned char>(name[a]))) ++a;
                        name = name.substr(a);
                        const std::string id = resolve_folder(target);
                        if (id.empty() || name.empty()) {
                            push_status("Usage: /chat folder rename <id|name> <new>");
                            return;
                        }
                        if (!conversation_store.rename_folder(id, name)) {
                            push_status("folder not found");
                            return;
                        }
                        push_status("folder renamed: " + name);
                        return;
                    }
                    if (fsub == "delete") {
                        std::string target;
                        iss >> target;
                        const std::string id = resolve_folder(target);
                        if (id.empty()) {
                            push_status("Usage: /chat folder delete <id|name>");
                            return;
                        }
                        if (!conversation_store.delete_folder(id)) {
                            push_status("folder not found");
                            return;
                        }
                        push_status("folder deleted (conversations unfiled)");
                        return;
                    }
                    if (fsub == "move") {
                        std::string conv_arg;
                        std::string folder_arg;
                        iss >> conv_arg >> folder_arg;
                        const std::string cid = resolve_chat_target(conv_arg);
                        if (cid.empty() || folder_arg.empty()) {
                            push_status("Usage: /chat folder move <conv> <folder|unfiled>");
                            return;
                        }
                        std::string fid;
                        if (folder_arg != "unfiled" && folder_arg != "-") {
                            fid = resolve_folder(folder_arg);
                            if (fid.empty()) {
                                push_status("folder not found (use a folder id/name or 'unfiled')");
                                return;
                            }
                        }
                        if (!conversation_store.move_to_folder(cid, fid)) {
                            push_status("move failed");
                            return;
                        }
                        push_status(fid.empty() ? "moved to unfiled" : "moved to folder [" + fid + "]");
                        return;
                    }
                    push_status("Usage: /chat folder list|new|rename|delete|move");
                    return;
                }
                push_status("Usage: /chat list|new|switch|search|title|delete|purge|folder");
                return;
            }
            if (cmd == "verbose") {
                std::string arg;
                iss >> arg;
                if (arg == "on")        cfg.verbose = true;
                else if (arg == "off")  cfg.verbose = false;
                else if (arg.empty())   cfg.verbose = !cfg.verbose;
                else {
                    push_status("Usage: /verbose [on|off]");
                    return;
                }
                push_status(std::string("verbose: ") +
                                      (cfg.verbose ? "on" : "off"));
                return;
            }
                if (cmd == "theme") {
                std::string arg;
                iss >> arg;
                if (arg.empty()) {
                    auto themes = arbiter::tui_list_available_themes(dir);
                    if (themes.empty()) {
                        push_status("ERR: no themes found");
                        return;
                    }
                    std::string active = arbiter::tui_active_preset();
                    if (active.empty()) {
                        // theme_file path → try basename stem so the picker
                        // lands on the matching user theme when present.
                        const std::string file = arbiter::tui_active_theme_file();
                        if (!file.empty()) {
                            const auto slash = file.find_last_of("/\\");
                            const std::string base = (slash == std::string::npos)
                                ? file : file.substr(slash + 1);
                            if (base.size() > 5 && base.ends_with(".json")) {
                                active = base.substr(0, base.size() - 5);
                            }
                        }
                    }
                    overlay_menu.open(
                        MenuPurpose::Theme,
                        "Themes",
                        "\u2191\u2193 preview  Enter select  Esc cancel",
                        menu_items_themes(themes, active),
                        active,
                        /*context=*/{},
                        /*max_width=*/48);
                    // Preview the landed-on theme, then dim so the menu lifts.
                    const std::string preview = overlay_menu.selected_item().id;
                    if (!preview.empty()) load_tui_design(dir, preview);
                    tui_begin_modal_dim();
                    refresh_chrome();
                    return;
                }
                if (arg == "list") {
                    const std::string active_preset = arbiter::tui_active_preset();
                    const std::string active_file = arbiter::tui_active_theme_file();
                    std::ostringstream out;
                    out << "Themes";
                    if (!active_preset.empty()) {
                        out << " (active preset: " << active_preset << ")";
                    } else if (!active_file.empty()) {
                        out << " (active file: " << active_file << ")";
                    }
                    out << ":\n";
                    for (const auto& preset : arbiter::tui_list_available_themes(dir)) {
                        out << "  " << preset;
                        if (preset == active_preset) out << "  *";
                        out << '\n';
                    }
                    out << "\nUsage:\n"
                           "  /theme                    — browse themes (↑↓ preview, Enter select)\n"
                           "  /theme <preset>           — built-in or ~/.arbiter/themes/<name>.json\n"
                           "  /theme save <name>        — export current look to themes/<name>.json\n"
                           "  /theme file <path>        — load theme JSON (sets theme_file in tui.json)\n"
                           "\nConfig (~/.arbiter/tui.json):\n"
                           "  { \"preset\": \"nord\" }   or   { \"theme_file\": \"themes/mine.json\" }\n"
                           "Override any token with bg/text/accent/border/content groups (#RRGGBB).\n"
                           "Export a starter: arbiter --export-theme high-contrast > ~/.arbiter/themes/mine.json";
                    push_status(out.str());
                    return;
                }
                if (arg == "save") {
                    std::string name;
                    iss >> name;
                    if (name.empty()) {
                        push_status("Usage: /theme save <name>");
                        return;
                    }
                    namespace fs = std::filesystem;
                    const std::string themes_dir = arbiter::tui_themes_dir(dir);
                    fs::create_directories(themes_dir);
                    const std::string path = themes_dir + "/" + name + ".json";
                    const std::string preset_hint = arbiter::tui_active_preset();
                    if (!arbiter::tui_write_theme_file(path,
                                                       arbiter::tui_design(),
                                                       preset_hint)) {
                        push_status("ERR: could not write " + path);
                        return;
                    }
                    push_status("saved theme: " + path);
                    return;
                }
                if (arg == "file") {
                    std::string path_arg;
                    iss >> path_arg;
                    if (path_arg.empty()) {
                        push_status("Usage: /theme file <path>\n"
                                            "  Path is relative to ~/.arbiter/ unless absolute.");
                        return;
                    }
                    if (!arbiter::set_tui_theme_file(dir, path_arg)) {
                        push_status("ERR: could not load theme file '" + path_arg + "'");
                        return;
                    }
                    refresh_chrome();
                    push_status("theme file: " + path_arg);
                    return;
                }
                if (!arbiter::tui_theme_name_is_valid(dir, arg)) {
                    push_status("ERR: unknown theme '" + arg + "' (/theme list)");
                    return;
                }
                arbiter::set_tui_preset(dir, arg);
                refresh_chrome();
                push_status("theme: " + arg);
                return;
            }

            {
                if (is_remote()) {
                    push_status(
                        "ERR: /" + cmd + " is not available in remote "
                        "(--connect) mode — run it on the API host or use "
                        "an HTTP-backed equivalent");
                    return;
                }
                std::string result = orch.execute_slash_command(line, current_agent);
                if (!result.empty()) {
                    push_status(result);
                    return;
                }
            }

            push_status("Unknown command. /help for list.");
            return;
        }

        // Plain text → stream to current agent
        reveal_sidebar();
        try {
            tool_indicator.begin();
            pane.last_interim_agent.clear();

            // First prompt of an untitled chat: name it immediately from the
            // user text, and kick a model title job in parallel with the turn
            // so the sidebar does not sit on "Untitled" until the reply ends.
            if (!is_remote()) {
                const std::string conv_id = pane.conversation_id;
                if (!conv_id.empty() && !conversation_store.is_titled(conv_id)) {
                    bool untitled = false;
                    for (const auto& e : conversation_store.list()) {
                        if (e.id != conv_id) continue;
                        untitled = (e.title == "Untitled");
                        break;
                    }
                    if (untitled) {
                        const std::string det =
                            arbiter::deterministic_conversation_title(line);
                        if (!det.empty()) {
                            conversation_store.set_title(conv_id, det);
                            if (pump_notify) pump_notify();
                            std::string title_model =
                                arbiter::load_title_model_override(dir);
                            if (title_model.empty()) {
                                title_model = orch.get_agent_model("index");
                            }
                            conversation_store.enqueue_title_job(
                                conv_id, line, /*assistant_msg=*/{},
                                title_model, orch);
                        }
                    }
                }
            }

            if (is_remote()) {
                auto resp = run_remote_turn(pane, line);
                tool_indicator.finalize();
                output_queue.end_message();
                pane.last_response = resp.ok ? resp.content
                                             : ("ERR: " + resp.error);
                if (!resp.ok) {
                    output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                } else {
                    // Best-effort title refresh after first turn.
                    remote_refresh_conversations();
                    refresh_history_sidebar_entries();
                    if (resp.input_tokens + resp.output_tokens > 0) {
                        sidebar.record_turn(current_agent, pane.current_model, resp);
                    }
                }
                thinking.stop();
                return;
            }

            arbiter::StreamRenderer renderer(master_stream_policy(cfg), output_queue);
            auto resp = orch.send_streaming(current_agent, line,
                [&](const std::string& chunk) { renderer.feed(chunk); },
                pane.original_task);
            renderer.flush();
            // Per-tool ToolSegment rows already reflect the turn; finalize
            // only clears the mid-separator spinner.
            tool_indicator.finalize();
            // md.flush() guarantees the stream ended on `\n`; one more gives
            // exactly one blank line before the next message.
            output_queue.end_message();
            // Stash the raw agent response (or error) so start_pane_thread's
            // post-handle hook can flow it back to the parent when this is
            // a delegated pane.  Written regardless of resp.ok so the
            // parent sees the failure, not silence.
            pane.last_response = resp.ok ? resp.content
                                         : ("ERR: " + resp.error);
            update_pane_original_task(pane, line, resp);
            try {
                auto note = orch.get_agent(current_agent).take_compaction_notice();
                if (!note.empty()) push_status(note);
            } catch (...) {}
            if (!resp.ok) {
                output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
            }
            // Durable per-turn autosave: coalesces onto the store's
            // background thread so a crash never loses more than the
            // in-flight turn, without stalling the input loop on JSON I/O.
            conversation_store.save_async(pane.conversation_id, orch);
        } catch (const std::exception& e) {
            output_queue.push_prose_msg("ERR: " + std::string(e.what()), StyleId::Error);
            pane.last_response = std::string("ERR: ") + e.what();
            conversation_store.save_async(pane.conversation_id, orch);
        }
        thinking.stop();
    // end handle_line
}

}  // namespace arbiter
