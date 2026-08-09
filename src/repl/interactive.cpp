#include "cli.h"
#include "repl/session.h"
#include "repl/session_internal.h"

#include "cli_helpers.h"
#include "orchestrator.h"
#include "agent_conversation.h"
#include "api_server.h"
#include "tenant_store.h"
#include "scheduler.h"
#include "notification_bus.h"
#include "remote/api_client.h"
#include "remote/connect_config.h"
#include "repl/layout.h"
#include "repl/layout_snapshot.h"
#include "repl/pane_history.h"
#include "repl/conversation_store.h"
#include "repl/transcript_replay.h"
#include "tui/tui.h"
#include "tui/tui_design.h"
#include "tui/tty_guard.h"
#include "tui/opentui/session.h"
#include "tui/opentui/sidebar_frame.h"
#include "tui/opentui/history_sidebar_frame.h"
#include "tui/opentui/theme_picker_frame.h"
#include "tui/sidebar.h"
#include "tui/history_sidebar.h"
#include "theme.h"
#include "config.h"

#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;

namespace arbiter {

ReplSession::ReplSession(std::string config_dir,
                         std::map<std::string, std::string> api_keys,
                         bool exec_allowed_flag)
    : dir(std::move(config_dir))
    , orch(std::move(api_keys))
    , conversation_store(dir)
{
    cfg.exec_allowed = exec_allowed_flag;
    orch.set_exec_disabled(!cfg.exec_allowed);
    orch.load_agents(dir + "/agents");

    history_sidebar.set_enabled(
        tui_design().layout.show_history_sidebar, dir);

    interactive_prompts.set_notify([this]() { wake_main_input(); });

    palette_items = {
        {"/ask",          "ask the index master"},
        {"/send",         "send a message to a specific agent"},
        {"/use",          "switch the focused pane's current agent"},
        {"/agents",       "list loaded agents"},
        {"/status",       "orchestrator status"},
        {"/tokens",       "token usage report"},
        {"/create",       "create agent with default config"},
        {"/remove",       "remove agent"},
        {"/reset",        "clear an agent's history"},
        {"/compact",      "summarize older turns to free context"},
        {"/model",        "list catalogue or change agent model"},
        {"/pane",         "spawn a parallel pane running an agent"},
        {"/find",         "search the focused pane's scrollback"},
        {"/loop",         "run agent in a background loop"},
        {"/loops",        "list running / suspended loops"},
        {"/log",          "show buffered loop output"},
        {"/watch",        "follow loop output"},
        {"/kill",         "stop a loop"},
        {"/suspend",      "pause a loop"},
        {"/resume",       "resume a paused loop"},
        {"/inject",       "inject a message into a running loop"},
        {"/fetch",        "fetch URL, send readable text to agent"},
        {"/browse",       "fetch + extract readable text"},
        {"/search",       "web search"},
        {"/mem",          "structured memory + scratchpad"},
        {"/todo",         "todo tracker"},
        {"/schedule",     "schedule recurring/one-shot tasks"},
        {"/exec",         "shell (host confirm / optional Docker sandbox)"},
        {"/diff",         "review/apply/reject/undo streamed ```diff patches"},
        {"/write",        "write a file"},
        {"/read",         "conversation artifacts"},
        {"/list",         "list conversation artifacts"},
        {"/mcp",          "MCP server registry"},
        {"/a2a",          "remote A2A agents"},
        {"/lesson",       "agent-scoped lessons"},
        {"/plan",         "execute a planner-produced plan file"},
        {"/theme",        "browse themes (↑↓ preview, Enter select)"},
        {"/verbose",      "toggle raw /cmd line streaming"},
        {"/chat list",    "list conversations"},
        {"/chat new",     "start a new conversation"},
        {"/chat switch",  "switch conversation"},
        {"/chat search",  "find text across saved conversations"},
        {"/chat title",   "rename the active conversation"},
        {"/chat folder",  "list/new/rename/delete/move folders"},
        {"/help",         "command reference"},
        {"/quit",         "exit"},
    };
}

ReplSession::~ReplSession() = default;

void ReplSession::wake_main_input() {
    deferred_main_interrupt.store(true, std::memory_order_release);
    if (auto* ed = active_readline.load(std::memory_order_acquire)) {
        ed->interrupt();
        return;
    }
    if (!layout_ptr) return;
    // try_lock: never block if main already holds layout_mu (e.g. chord
    // dispatch / present). Join no longer holds the lock, but other
    // paths still do.
    std::unique_lock<std::recursive_mutex> lk(layout_mu, std::try_to_lock);
    if (lk) layout_ptr->focused().editor.interrupt();
}

void ReplSession::fail_pending_prompts() {
    interactive_prompts.fail_all(InteractiveDecision::Cancel);
}

Rect ReplSession::layout_bounds() {
    const int cols = term_cols();
    const int rows = term_rows();
    const int leading = HistorySidebarState::width_for_terminal(
        cols, history_sidebar.enabled());
    const int panes = layout_ptr ? static_cast<int>(layout_ptr->pane_count()) : 1;
    const int session_w = sidebar.effective_width(cols, panes, leading);
    // Reserve a trailing gutter so the session box isn't flush to the edge.
    const int trailing = session_w > 0
        ? session_w + SidebarState::kOuterGutter
        : 0;
    // Full terminal height — no top header bar.
    return Rect{leading, 0, std::max(1, cols - leading - trailing), std::max(1, rows)};
}

int64_t ReplSession::parse_conversation_db_id(const std::string& id) {
    if (id.empty()) return 0;
    char* end = nullptr;
    const long long v = std::strtoll(id.c_str(), &end, 10);
    if (end == id.c_str() || *end != '\0' || v <= 0) return 0;
    return static_cast<int64_t>(v);
}

void ReplSession::bind_tools_conversation(const std::string& conv_id) {
    const int64_t cid = parse_conversation_db_id(conv_id);
    if (!tool_conversation_id) {
        tool_conversation_id = std::make_shared<std::atomic<int64_t>>(cid);
    } else {
        tool_conversation_id->store(cid, std::memory_order_relaxed);
    }
    set_tool_conversation_tls(cid);
}

void ReplSession::persist_layout() {
    if (!layout_ptr) return;
    const auto snap = layout_ptr->capture_snapshot();
    const std::string json = layout_snapshot_to_json(snap);
    conversation_store.tenant_store().set_tui_layout_json(
        conversation_store.tenant_id(), json);
    fs::create_directories(dir + "/conversations");
    if (!save_layout_snapshot(layout_snapshot_path(dir), snap)) {
        std::fprintf(stderr,
                     "[layout] failed to persist pane layout to %s\n",
                     layout_snapshot_path(dir).c_str());
    }
}

bool ReplSession::sync_layout_to_terminal() {
    const Rect want = layout_bounds();
    const Rect have = layout_ptr->outer_bounds();
    if (want.x == have.x && want.y == have.y
        && want.w == have.w && want.h == have.h) {
        return false;
    }
    layout_ptr->resize(want);
    layout_ptr->for_each_pane([&](Pane& p) {
        pane_history_set_cols(p, p.tui.cols());
    });
    return true;
}

void ReplSession::reveal_sidebar() {
    if (sidebar.session_started()) return;
    sidebar.mark_prompt_started();
    std::lock_guard<std::recursive_mutex> lk(layout_mu);
    sync_layout_to_terminal();
    refresh_focused_input.store(true);
    layout_ptr->focused().editor.interrupt();
}

void ReplSession::refresh_chrome() {
    std::lock_guard<std::recursive_mutex> lk(layout_mu);
    layout_ptr->for_each_pane([&](Pane& p) { pane_history_retheme(p); });
    ot_session.apply_design();
    if (ui_ctx.present_all) ui_ctx.present_all();
    ot_session.flush_display();
    refresh_focused_input.store(true);
    layout_ptr->focused().editor.interrupt();
}

int ReplSession::outer_bottom_input_rows() {
    int rows = 0;
    layout_ptr->for_each_pane([&](Pane& p) {
        const TuiChromeSnapshot chrome = p.tui.chrome_snapshot();
        if (chrome.outer_bottom) {
            rows = std::max(rows, chrome.input_rows);
        }
    });
    return rows;
}

void ReplSession::present_unlocked() {
    pane_history_present(ui_ctx, pane_hooks);
}

void ReplSession::present_holding_lock() {
    if (sync_layout_to_terminal()) refresh_focused_input.store(true);
    present_unlocked();
}

void ReplSession::setup_pane_hooks() {
    pane_hooks.for_each_pane = [this](const std::function<void(Pane&)>& fn) {
        layout_ptr->for_each_pane(fn);
    };
    pane_hooks.draw_overlays = [this](OpenTuiHandle frame, int cols, int rows) {
        if (frame == 0 || cols <= 0 || rows <= 0) return;

        // Align both sidebars to the layout's outer bottom chrome, not the
        // focused pane — mid-column focus must not shorten Conversations /
        // Session relative to the column bottoms.
        const Rect outer = layout_ptr->outer_bounds();
        const int outer_bottom_pad =
            tui_outer_bottom_pad_rows(tui_design());
        const int sidebar_input_rows = outer_bottom_input_rows();

        const Rect hb = HistorySidebarState::rect_for_terminal(
            cols, rows, history_sidebar.enabled());
        if (hb.w > 0) {
            // Avoid reloading the store mid-edit — refresh is unnecessary
            // while the user types a rename / navigates an overlay.
            HistorySidebarSnapshot hs = history_sidebar.snapshot();
            if (!hs.renaming && !hs.moving
                && !hs.menu_open && !hs.confirming_delete) {
                refresh_history_sidebar_entries();
                hs = history_sidebar.snapshot();
            }
            hs.active_id = conversation_active_id();
            opentui::draw_history_sidebar(
                frame, hs, hb, outer, sidebar_input_rows, outer_bottom_pad);
        }

        if (layout_ptr->pane_count() > 1) layout_ptr->draw_borders(frame);

        if (theme_picker.active()) {
            opentui::draw_theme_picker(
                frame, theme_picker.snapshot(), layout_ptr->focused().tui);
        }

        const int panes = static_cast<int>(layout_ptr->pane_count());
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        int sw = sidebar.effective_width(cols, panes, leading);
        if (sw <= 0) return;

        int pane_x = outer.x;
        int pane_w = outer.w;
        int gap = cols - pane_x - pane_w;
        // Trailing gutter is reserved in layout_bounds; keep the box width at sw.
        if (sw <= 0 || gap < sw) return;

        const Rect sb = {pane_x + pane_w, 0, sw, std::max(1, rows)};
        Pane& focused = layout_ptr->focused();
        sidebar.set_focus_context(focused.current_agent,
                                  focused.current_model);
        sidebar.set_active_tool_calls(focused.tool_indicator.total());
        std::vector<SidebarLoopEntry> loop_rows;
        for (const auto& b : loops.briefs()) {
            SidebarLoopEntry row;
            row.id       = b.id;
            row.agent_id = b.agent_id;
            row.state    = b.state;
            row.iter     = b.iter;
            loop_rows.push_back(std::move(row));
        }
        sidebar.set_loops(std::move(loop_rows));
        const SidebarSnapshot snap = sidebar.snapshot();
        opentui::draw_sidebar(
            frame, snap, sb, outer, sidebar_input_rows, outer_bottom_pad);
    };
}

void ReplSession::boot_layout_and_transcripts(bool restored) {
    layout_holder = std::make_unique<LayoutTree>(
        make_pane(), layout_bounds());
    layout_ptr = layout_holder.get();

    if (is_remote()) {
        // Remote sessions are single-pane for v1 (no local layout.json).
        const std::string active = conversation_active_id();
        layout_ptr->for_each_pane([&](Pane& p) {
            p.conversation_id = active;
            p.current_agent = "index";
            p.current_model = "remote";
            pane_history_set_cols(p, p.tui.cols());
        });
        if (!active.empty()) {
            apply_conversation_to_pane(layout_ptr->focused(), active,
                                       /*replay=*/true);
        }
        refresh_history_sidebar_entries();
        return;
    }

    // Restore multi-pane layout + per-pane conversation bindings (#42).
    // Prefer the SQLite tui_prefs copy; fall back to the legacy file path.
    // Missing/deleted conversation ids fall back to the store's active id;
    // corrupt or oversized snapshots keep the single-pane default.
    {
        std::optional<LayoutSnapshot> snap;
        const std::string prefs_json =
            conversation_store.tenant_store().get_tui_layout_json(
                conversation_store.tenant_id());
        if (!prefs_json.empty()) snap = layout_snapshot_from_json(prefs_json);
        if (!snap) snap = load_layout_snapshot(layout_snapshot_path(dir));
        if (snap) {
            std::unordered_set<std::string> known;
            for (const auto& e : conversation_store.list()) known.insert(e.id);
            const std::string fallback = conversation_store.active_id();
            for_each_layout_leaf(snap->root, [&](LayoutSnapshot::Node& leaf) {
                if (leaf.conversation_id.empty() ||
                    !known.count(leaf.conversation_id)) {
                    leaf.conversation_id = fallback;
                }
                if (leaf.agent.empty()) leaf.agent = "index";
            });
            if (validate_layout_snapshot(*snap) &&
                layout_ptr->restore_snapshot(
                    *snap,
                    [this]() { return make_pane(); },
                    layout_bounds())) {
                // layout_bounds() above used pane_count==1 (pre-restore). The
                // session sidebar hides when pane_count > 1, so recompute now
                // before col sync / transcript replay.
                layout_ptr->resize(layout_bounds());
            }
        }
    }

    layout_ptr->for_each_pane([&](Pane& p) {
        pane_history_set_cols(p, p.tui.cols());
        if (p.current_agent != "index" && !orch.has_agent(p.current_agent)) {
            p.current_agent = "index";
        }
        p.current_model = orch.get_agent_model(p.current_agent);
    });

    // Load each open conversation into orch (active may already be loaded)
    // and replay transcript tails into panes so relaunch matches the prior
    // multi-pane arrangement.  Each (conversation_id, agent) binding is
    // replayed once — live ^W splits share a conversation but start with
    // empty scrollback, so replaying into every sibling would pollute those
    // empty windows with the parent transcript.
    {
        std::set<std::string> loaded_ids;
        std::unordered_set<std::string> replayed_bindings;
        bool any_history = restored;
        layout_ptr->for_each_pane([&](Pane& pane) {
            const std::string& id = pane.conversation_id;
            if (id.empty()) return;
            if (loaded_ids.insert(id).second) {
                if (!orch.has_conversation_loaded(id)) {
                    if (conversation_store.load(id, orch)) any_history = true;
                }
            }
            const std::string& agent = pane.current_agent.empty()
                ? "index" : pane.current_agent;
            if (!claim_pane_transcript_replay(replayed_bindings, id, agent)) {
                return;
            }
            ConversationScope scope(id);
            // Replay the pane's bound agent (restored from layout.json), not
            // always index — non-index panes would otherwise show the wrong
            // transcript after relaunch.
            const auto history = orch.get_agent_history(agent);
            const size_t total = history.size();
            if (total > 0) {
                any_history = true;
                replay_transcript(
                    pane, history, replay_tail_begin(total), total);
            }
        });
        if (!layout_ptr->focused().conversation_id.empty()) {
            conversation_store.set_active(layout_ptr->focused().conversation_id);
            bind_tools_conversation(layout_ptr->focused().conversation_id);
        }
        if (any_history) sidebar.mark_prompt_started();
    }
}

void ReplSession::shutdown() {
    // Fail any confirm/diff waiters before joining so exec threads cannot
    // stay blocked in fut.get() while we wait on join.
    fail_pending_prompts();
    // Stop every pane's queue under layout_mu, move threads out, then join
    // *outside* the lock so an exec thread blocked on layout_mu (spawn /
    // present / find) can finish.  After that the pump is the only producer
    // left and we can shut it down too.
    std::vector<std::thread> exec_joins;
    {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        layout_ptr->for_each_pane([&](Pane& p) {
            p.cmd_queue.stop();
            cancel_pane_turn(p);
            if (p.exec_thread.joinable()) {
                exec_joins.push_back(std::move(p.exec_thread));
            }
        });
    }
    for (auto& t : exec_joins) {
        if (t.joinable()) t.join();
    }

    pump_stop = true;
    pump_cv.notify_one();   // unblock the pump's wait_for so it exits promptly
    if (output_pump.joinable()) output_pump.join();

    g_ui_ctx = nullptr;
    ot_session.shutdown();
    // Keep g_tui_armed until StdinRawModeGuard restores termios so a fatal
    // signal in this window still emits the full emergency reset. Cleared
    // just before the guard runs at scope exit (after history persist).
    // StdinRawModeGuard drains stdin and restores cooked mode on destruction.

    // Persist the shared history, dropping duplicates while preserving
    // last-seen order (all panes share one store, so no merge needed).
    {
        std::ofstream hf(get_config_dir() + "/history");
        std::vector<std::string> merged;
        std::set<std::string> seen;
        for (auto& h : shared_history->snapshot())
            if (seen.insert(h).second) merged.push_back(h);
        for (auto& h : merged) hf << h << '\n';
    }
    // Drain any autosave still in flight, then save every distinct open
    // pane conversation (and fall back to the store's active id). Persist
    // the pane layout last so relaunch restores the same split tree (#42).
    // Remote sessions own conversation state on the API host — skip local
    // tenants.db writes.
    if (!is_remote()) {
        conversation_store.flush();
        {
            std::set<std::string> saved;
            layout_ptr->for_each_pane([&](Pane& p) {
                if (p.conversation_id.empty() || !saved.insert(p.conversation_id).second) return;
                conversation_store.save(p.conversation_id, orch);
            });
            if (saved.empty()) {
                conversation_store.save(conversation_store.active_id(), orch);
            } else {
                conversation_store.set_active(layout_ptr->focused().conversation_id);
            }
        }
        persist_layout();
    }

    if (scheduler) scheduler->stop();

    ::write(STDOUT_FILENO, "\n", 1);
    // StdinRawModeGuard disarms g_tui_armed and restores cooked mode at
    // scope exit (and on exception unwind).
}

void ReplSession::run() {
    // ── Raw stdin + OpenTUI session ────────────────────────────────────────
    // Must happen before Session::start() below: setupTerminal() sends
    // capability queries (OSC 10/11, DECRQM, DA/XTVERSION, CPR) and the
    // terminal's replies land on stdin. With ECHO still enabled (inherited
    // cooked mode), the kernel line discipline echoes those bytes to the
    // screen the instant they arrive — reading them later doesn't undo
    // that. Raw mode has to be in effect *before* the queries go out.
    // stdin_guard is a member declared before ot_session so exception unwind
    // restores termios after OpenTUI teardown (reverse destruction order).
    ot_session.start(static_cast<std::uint32_t>(term_cols()),
                     static_cast<std::uint32_t>(term_rows()));
    // Install fatal handlers BEFORE enabling mouse / arming so a crash or
    // SIGTERM during the rest of startup cannot leave sticky DEC modes or
    // raw termios in the host shell.
    install_tui_fatal_handlers();
    g_tui_armed = 1;
    // Button+drag+wheel tracking without any-event motion (?1003).
    if (tui_design().layout.mouse) {
        ot_session.engine().set_mouse_enabled(true, /*enable_movement=*/false);
    }
    if (stdin_guard.active) drain_stdin_spurious(200);
    ui_ctx.session = &ot_session;
    g_ui_ctx = &ui_ctx;

    bool restored = false;
    if (!is_remote()) {
        restored = conversation_store.load(conversation_store.active_id(), orch);
        if (restored) sidebar.mark_prompt_started();

        // TUI chat history and tool scoping share tenants.db via ConversationStore.
        TenantStore& tenants = conversation_store.tenant_store();
        const int64_t primary_tenant_id = conversation_store.tenant_id();
        Tenant primary = tenants.get_tenant(primary_tenant_id)
                             .value_or(Tenant{});
        if (primary.id == 0) {
            primary = ensure_primary_tenant(tenants);
        }

        // Live conversation id for tool callbacks. Wired once; turns update
        // the atomic so /todo|/mem|artifacts follow the executing pane without
        // reinstalling callbacks mid-sibling-turn.
        tool_conversation_id = std::make_shared<std::atomic<int64_t>>(
            parse_conversation_db_id(conversation_store.active_id()));
        api_opts = make_cli_api_options(dir, get_api_keys(), cfg.exec_allowed);
        // Opt-in Docker sandbox for interactive /exec.  When the image is
        // set and usable, /exec runs in a per-tenant container; host shell
        // remains the default (confirm-gated) when the sandbox is unset.
        // If the image was requested but unusable, refuse the host path —
        // same honesty contract as `--api`.
        if (api_opts.sandbox_enabled) {
            SandboxConfig sc;
            sc.runtime              = api_opts.sandbox_runtime;
            sc.image                = api_opts.sandbox_image;
            sc.workspaces_root      = api_opts.sandbox_workspaces_root;
            sc.network              = api_opts.sandbox_network;
            sc.memory_mb            = api_opts.sandbox_memory_mb;
            sc.cpus                 = api_opts.sandbox_cpus;
            sc.pids_limit           = api_opts.sandbox_pids_limit;
            sc.exec_timeout_seconds = api_opts.sandbox_exec_timeout_seconds;
            sc.workspace_max_bytes  = api_opts.sandbox_workspace_max_bytes;
            sc.idle_seconds         = api_opts.sandbox_idle_seconds;
            sandbox_manager = std::make_unique<SandboxManager>(std::move(sc));
            if (sandbox_manager->usable()) {
                api_opts.sandbox = sandbox_manager.get();
                api_opts.exec_disabled = false;
                api_opts.host_exec_enabled = false;
            } else {
                std::cerr << "WARN: TUI sandbox disabled — "
                          << sandbox_manager->unusable_reason()
                          << "\n      /exec returns ERR until fixed; unset "
                             "ARBITER_SANDBOX_IMAGE to use host shell.\n";
                // Keep sandbox_enabled so make_exec_invoker_callback does
                // not silently fall back to host popen, and force
                // exec_disabled so the dispatcher cannot reach cmd_exec
                // via the "no invoker but exec allowed" host path.
                api_opts.sandbox = nullptr;
                api_opts.exec_disabled = true;
                api_opts.host_exec_enabled = false;
                cfg.exec_allowed = false;
                orch.set_exec_disabled(true);
            }
        }
        wire_orchestrator_tools(orch, api_opts, tenants, primary.id,
                                tool_conversation_id);
        // API wiring installs a capture-only /write interceptor (no host cwd).
        // The interactive TUI persists to the active conversation's bound
        // workspace directory after confirm — never the process launch cwd.
        orch.set_write_interceptor(nullptr);
        orch.set_workspace_root_provider([this]() -> std::string {
            // Pane exec threads enter ConversationScope(p.conversation_id)
            // before handle_line; that key is the authoritative binding.
            const std::string& id = arbiter::agent_conversation_key();
            if (id.empty()) return {};
            return conversation_store.resolved_workspace_root(id);
        });

        scheduler = std::make_unique<Scheduler>(&api_opts, &tenants, &notifications);
        scheduler->start();
    } else {
        // Remote: conversations already bootstrapped in cmd_interactive_remote.
        sidebar.mark_prompt_started();
        sidebar.set_remote_info(remote->config().display_host, remote_tenant_name);
        // Ensure active id is set for pane bind.
        if (remote_active_id.empty() && !remote_conversations.empty()) {
            remote_active_id = remote_conversations.front().id;
        }
        // Pane exec threads call bind_tools_conversation each turn — keep a
        // live atomic even though remote mode does not wire local tools.
        tool_conversation_id = std::make_shared<std::atomic<int64_t>>(
            parse_conversation_db_id(remote_active_id));
    }

    // Load input history into one live store shared by every pane's editor:
    // a command typed in any pane is instantly in every pane's Up-arrow /
    // Ctrl-R history.
    shared_history = std::make_shared<opentui::SharedInputHistory>();
    {
        std::vector<std::string> loaded;
        std::ifstream hf(get_config_dir() + "/history");
        std::string line;
        while (std::getline(hf, line))
            if (!line.empty()) loaded.push_back(std::move(line));
        shared_history->replace(std::move(loaded));
    }

    std::cout.flush();
    install_sigwinch_handler();

    install_orch_callbacks();
    boot_layout_and_transcripts(restored);
    setup_pane_hooks();

    ui_ctx.present_all = [this]() {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        present_holding_lock();
    };

    present_unlocked();

    // Exec-capability warning — list any agents that can run shell commands.
    // Queued here so the pump thread renders it on its first tick.
    // Remote sessions orchestrate on the API host — skip local exec notice.
    if (cfg.exec_allowed && !is_remote()) {
        std::vector<std::string> exec_agents;
        for (const auto& id : orch.list_agents_all()) {
            for (const auto& cap : orch.get_constitution(id).capabilities) {
                if (cap == "exec") { exec_agents.push_back(id); break; }
            }
        }
        if (!exec_agents.empty()) {
            std::string names;
            for (size_t i = 0; i < exec_agents.size(); ++i) {
                if (i) names += ", ";
                names += exec_agents[i];
            }
            layout_ptr->focused().output_queue.push_prose_msg(
                "[exec enabled: " + names +
                " \xe2\x80\x94 shell commands will run as you]",
                StyleId::System);
        }
    }

    g_getc_state.pane = &layout_ptr->focused();
    ui_ctx.focused_pane = &layout_ptr->focused();

    orch.set_pane_spawner([this](const std::string& agent, const std::string& msg) {
        return spawn_pane(agent, msg);
    });

    start_output_pump();
    run_input_loop();
    shutdown();
}

void cmd_interactive(bool exec_allowed_flag, std::string_view theme_override) {
    std::string dir = get_config_dir();
    load_tui_design(dir, theme_override);
    auto api_keys = get_api_keys();

    ReplSession session(std::move(dir), std::move(api_keys), exec_allowed_flag);
    session.run();
}

void cmd_interactive_remote(RemoteConnectConfig cfg, bool exec_allowed_flag,
                            std::string_view theme_override) {
    std::string dir = get_config_dir();
    load_tui_design(dir, theme_override);

    // Fail-fast bootstrap before raw terminal mode / OpenTUI.
    RemoteApiClient client(cfg);
    auto boot = client.bootstrap();
    if (!boot.ok) {
        std::cerr << "ERR: " << boot.error << "\n";
        std::cerr << "  Tried " << cfg.base_url << "\n";
        std::exit(1);
    }

    ReplSession session(std::move(dir), std::move(cfg), exec_allowed_flag);
    {
        std::lock_guard<std::mutex> lk(session.remote_conv_mu);
        session.remote_conversations = std::move(boot.conversations);
        session.remote_active_id = boot.active_conversation_id;
    }
    if (!boot.tenant_name.empty()) {
        session.remote_tenant_name = boot.tenant_name;
        session.sidebar.set_remote_info(
            session.remote->config().display_host, boot.tenant_name);
    }
    session.run();
}

}  // namespace arbiter
