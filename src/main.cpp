// Usage:
//   arbiter                          — interactive REPL
//   arbiter --api [--port 8080]      — HTTP+SSE multi-tenant API server
//   arbiter --send <agent> <msg>     — one-shot message
//   arbiter --init                   — create config dir + example agents

#include "orchestrator.h"
#include "commands.h"
#include "constitution.h"
#include "readline_wrapper.h"
#include "markdown.h"
#include "cli_helpers.h"
#include "repl/queues.h"
#include "loop_manager.h"
#include "title_generator.h"
#include "cli.h"
#include "tui/tui.h"
#include "tui/line_editor.h"
#include "tui/stream_filter.h"
#include "repl/pane.h"
#include "repl/layout.h"
#include "theme.h"
#include "config.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>

namespace fs = std::filesystem;

using arbiter::BANNER;
using arbiter::agent_color;
using arbiter::get_config_dir;
using arbiter::get_memory_dir;
using arbiter::get_api_keys;
using arbiter::write_memory;
using arbiter::read_memory;
using arbiter::fetch_url;
using arbiter::theme;


// ─── Terminal / TUI ──────────────────────────────────────────────────────────

static volatile sig_atomic_t g_winch = 0;
static void sigwinch_handler(int) { g_winch = 1; }


using arbiter::TUI;
using arbiter::ThinkingIndicator;
using arbiter::ToolCallIndicator;
using arbiter::StreamFilter;
using arbiter::CommandQueue;
using arbiter::OutputQueue;
using arbiter::LoopManager;
using arbiter::Pane;
using arbiter::LayoutTree;
using arbiter::Rect;

// State the output-pump thread needs.  Points into cmd_interactive()'s stack
// frame — lifetime is the REPL call.  Pane* is the only per-pane hook; the
// pump reads pane.output_queue / pane.history / pane.tui through it.
struct ReplGetcState {
    Pane*                     pane = nullptr;
};

static ReplGetcState g_getc_state;

// Set by each pane's exec thread at startup and left untouched thereafter.
// Orchestrator callbacks (progress/tool-status/agent-start/compact) run
// synchronously from whichever exec thread invoked orch.send_streaming, so
// reading this thread-local gets the owning pane with zero synchronization.
// Threads other than pane-exec (main, pump) leave it nullptr.
thread_local Pane* g_active_pane = nullptr;

// Drain any pending exec output, record it in the scroll buffer, and render.
// VT100 DECSTBM is gone now (it's a physical-terminal resource, unusable for
// multi-pane), so we always re-render the scroll region from ScrollBuffer —
// a full clear+repaint of ~40 rows per output tick is cheap and avoids the
// auto-scroll shared-state problem.
static void getc_flush_output() {
    auto& S = g_getc_state;
    if (!S.pane) return;
    Pane& pane = *S.pane;
    std::string pending = pane.output_queue.drain();
    if (pending.empty()) return;

    int before = (pane.scroll_offset > 0) ? pane.history.total_visual_rows() : 0;
    pane.history.push(pending);

    if (pane.scroll_offset > 0) {
        int after = pane.history.total_visual_rows();
        pane.new_while_scrolled += (after - before);
    }
    pane.tui.render_scrollback(pane.history, pane.scroll_offset,
                                pane.new_while_scrolled);
}


// ─────────────────────────────────────────────────────────────────────────────

static void cmd_interactive() {
    std::string dir = get_config_dir();
    auto api_keys = get_api_keys();

    arbiter::Orchestrator orch(std::move(api_keys));
    orch.set_memory_dir(get_memory_dir());
    orch.load_agents(dir + "/agents");

    // ── App-scope shared state ─────────────────────────────────────────────
    // /verbose flips cfg.verbose and the StreamFilter wrapping each agent
    // turn checks it on the fly — no need to rebuild the filter when the
    // flag changes.  Not persisted across sessions by design.
    arbiter::Config cfg;
    LoopManager loops;

    // Each pane's exec thread sets ::g_active_pane (file-scope thread_local)
    // at startup; orch callbacks read that thread's value to route output to
    // the right pane without any shared state.  Main and pump threads leave
    // g_active_pane nullptr — they don't handle() commands.

    // Serializes layout tree mutations (split/close/focus) against the pump
    // thread's iteration.  Recursive because dispatch_chord internally does
    // layout operations that call back into the tree.
    std::recursive_mutex layout_mu;

    struct ConfirmState {
        std::mutex            mu;
        std::string           prompt;
        std::promise<bool>*   pending = nullptr;
    } confirm_state;

    std::atomic<bool> quit_requested{false};
    std::atomic<bool> title_generated{false};   // generated once per session

    // Set by any worker thread that mutated the layout (pane spawner, pump
    // handling SIGWINCH) — signals the main thread to bounce out of its
    // blocked read_line and re-enter begin_input so the focused pane's
    // blank input row gets a fresh prompt painted over it.  Without this,
    // the prompt only reappears after the user types something.
    std::atomic<bool> refresh_focused_input{false};

    // Pending-close queue for the delegation chain.  A pane's exec thread
    // appends an entry after its spawn_message turn completes and it has
    // flowed its output back to its parent.  The main thread services the
    // queue between read_line iterations: prompts the user on the focused
    // pane ("close X?"), and on yes stops+joins the pane's thread and
    // removes it from the layout.  Non-blocking from the exec thread's
    // perspective — the pane's thread is already done with its task.
    struct PendingClose {
        Pane*       pane;
        std::string agent_id;  // for the prompt text
    };
    std::mutex                pending_closes_mu;
    std::vector<PendingClose> pending_closes;

    // Session files are scoped to the working directory so that starting
    // arbiter in different repos doesn't bleed history across contexts.
    auto cwd_session_key = []() -> std::string {
        std::string cwd = fs::current_path().string();
        uint32_t h = 2166136261u;
        for (unsigned char c : cwd) { h ^= c; h *= 16777619u; }
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", h);
        return buf;
    };
    std::string sessions_dir = dir + "/sessions";
    fs::create_directories(sessions_dir);
    std::string session_path = sessions_dir + "/" + cwd_session_key() + ".json";
    bool restored = orch.load_session(session_path);
    (void)restored;

    // Load shared input history once — each pane's LineEditor gets a copy.
    std::vector<std::string> shared_history;
    {
        std::ifstream hf(get_config_dir() + "/history");
        std::string line;
        while (std::getline(hf, line))
            if (!line.empty()) shared_history.push_back(std::move(line));
    }

    std::cout.flush();

    signal(SIGWINCH, sigwinch_handler);

    // ── Raw stdin ──────────────────────────────────────────────────────────
    struct termios orig_stdin_tm;
    bool stdin_is_tty = (::tcgetattr(STDIN_FILENO, &orig_stdin_tm) == 0);
    if (stdin_is_tty) {
        struct termios raw = orig_stdin_tm;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    TUI::enter_alt_screen();

    // Forward declaration — layout_ptr lets lambdas registered before layout
    // construction reach it safely (we set it once and never clear).
    LayoutTree* layout_ptr = nullptr;

    // Title-gen helper — fires once per session, paints into the pane that
    // triggered the response.
    auto maybe_generate_title = [&](const std::string& user_msg,
                                     const std::string& response_snippet) {
        if (title_generated.exchange(true)) return;
        Pane* target = g_active_pane;
        if (!target) return;
        TUI* target_tui = &target->tui;
        arbiter::generate_title_async(orch.client(), user_msg, response_snippet,
            [target_tui](const std::string& t){ target_tui->set_title(t); });
    };

    // ── Pane factory ───────────────────────────────────────────────────────
    // Layout calls this when splitting to materialise a new pane with every
    // callback wired to app-scope state.  Defined before orch callbacks so
    // their captures of active_pane/layout_ptr are consistent.
    auto make_pane = [&]() -> std::unique_ptr<Pane> {
        auto p = std::make_unique<Pane>();
        p->current_agent = "index";
        p->current_model = orch.get_agent_model(p->current_agent);
        p->tui.init(p->current_agent, p->current_model,
                    agent_color(p->current_agent));
        p->history.set_cols(p->tui.cols());

        p->editor.set_max_history(1000);
        p->editor.set_history(shared_history);  // copy per-pane

        p->editor.set_completion_provider(
            [&orch, &loops](const std::string& buf, const std::string& tok)
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
                                  "/pane",
                                  "/loop","/loops","/log","/watch",
                                  "/kill","/suspend","/resume","/inject",
                                  "/fetch","/mem","/plan","/verbose","/quit","/help"});
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
                if (cmd == "mem") return match({"write","read","show","clear"});
                return {};
            });

        Pane* raw = p.get();
        p->editor.set_scroll_handler([raw](int direction, int step) {
            int max_off = raw->history.total_visual_rows();
            if (direction < 0) {
                raw->scroll_offset = std::min(raw->scroll_offset + step, max_off);
            } else {
                raw->scroll_offset = std::max(0, raw->scroll_offset - step);
                raw->new_while_scrolled = 0;
                if (raw->scroll_offset == 0) raw->tui.clear_status();
            }
            raw->tui.render_scrollback(raw->history, raw->scroll_offset,
                                        raw->new_while_scrolled);
        });
        p->editor.set_cancel_handler([raw, &orch]() {
            orch.cancel();
            raw->multiline_accum.clear();
            raw->output_queue.push_msg(theme().accent_error + "[interrupted]" + theme().reset + "");
        });
        // Chord prefix: Ctrl-w.  Recognized follow-ups are w/s/v/c (+Ctrl-w
        // itself as a synonym for 'w'); anything else drops the chord
        // silently.  Dispatch happens in the REPL main loop via take_chord.
        p->editor.set_chord_handler([](char cmd) -> bool {
            return cmd == 'w' || cmd == 's' || cmd == 'v' || cmd == 'c'
                || cmd == 0x17;
        });
        return p;
    };

    // ── Orchestrator callbacks ─────────────────────────────────────────────
    // All pane-facing callbacks route through g_active_pane (thread-local),
    // which each pane's exec thread sets at startup.
    orch.set_progress_callback([&](const std::string& /*agent_id*/,
                                    const std::string& content) {
        Pane* p = g_active_pane;
        if (!p) return;
        const std::string& dim = theme().text_dimmer;
        const std::string& rst = theme().reset;
        std::string filtered;
        StreamFilter filter(cfg,
            [&filtered](const std::string& s) { filtered += s; });
        filter.feed(content);
        filter.flush();
        if (filtered.empty()) return;
        std::string buf;
        std::istringstream ss(filtered);
        std::string ln;
        while (std::getline(ss, ln)) {
            buf += dim; buf += "  "; buf += ln; buf += rst; buf += "\n";
        }
        p->output_queue.push_msg(buf);
    });
    orch.set_tool_status_callback([&](const std::string& kind, bool ok) {
        Pane* p = g_active_pane;
        if (p) p->tool_indicator.bump(kind, ok);
    });
    // No local cost callback in REPL mode — billing/cost reporting has
    // moved to an external billing service on the API path, and the
    // interactive REPL no longer prints per-turn cost summaries.
    orch.set_agent_start_callback([&](const std::string& agent_id) {
        Pane* p = g_active_pane;
        if (p) p->thinking.start(agent_id + ": thinking");
    });
    orch.set_compact_callback([&](const std::string& agent_id, size_t n) {
        Pane* p = g_active_pane;
        if (p) p->tui.set_status(agent_id + ": compacting context (" +
                                 std::to_string(n) + " msgs)");
    });
    // Advisor gate halt — surface as a distinct banner separate from the
    // agent's normal text stream.  HALT means the runtime stopped the
    // executor before it could return; the user needs to see the reason
    // out-of-band so it doesn't blend into the agent's last sentence.
    orch.set_escalation_callback([&](const std::string& agent_id,
                                      int /*stream_id*/,
                                      const std::string& reason) {
        Pane* p = g_active_pane;
        if (!p) return;
        std::string banner =
            theme().accent_error + "[advisor halt: " + agent_id + "] " +
            reason + theme().reset + "";
        p->output_queue.push_msg(banner);
    });

    // Advisor activity (consults + gate decisions) — render as dim status
    // lines so the user can watch the runtime gate without it crowding the
    // agent's actual output.  Continue events are silent (they're the
    // common case and wouldn't add information); only redirects, halts,
    // budget exhaustion, and explicit /advise consultations surface here.
    orch.set_advisor_event_callback([&](const arbiter::Orchestrator::AdvisorEvent& ev) {
        Pane* p = g_active_pane;
        if (!p) return;
        if (ev.kind == "gate_continue") return;  // quiet success
        const std::string& dim  = theme().text_dimmer;
        const std::string& warn = theme().accent_warning;
        const std::string& err  = theme().accent_error;
        const std::string& rst  = theme().reset;
        std::string label, color;
        if      (ev.kind == "consult")       { label = "advisor consult"; color = dim; }
        else if (ev.kind == "gate_redirect") { label = "advisor redirect"; color = warn; }
        else if (ev.kind == "gate_halt")     { label = "advisor halt";    color = err;  }
        else if (ev.kind == "gate_budget")   { label = "advisor budget";  color = err;  }
        else                                  { label = ev.kind;            color = dim; }
        std::string detail = ev.detail;
        if (detail.size() > 200) { detail.resize(197); detail += "..."; }
        std::string line = color + "[" + label + ": " + ev.agent_id + "]";
        if (!detail.empty()) line += " " + detail;
        line += rst + "";
        p->output_queue.push_msg(line);
    });

    orch.set_confirm_callback([&](const std::string& p) -> bool {
        std::promise<bool> done;
        auto fut = done.get_future();
        {
            std::lock_guard<std::mutex> lk(confirm_state.mu);
            confirm_state.prompt  = p;
            confirm_state.pending = &done;
        }
        // Wake the focused pane's editor so the main loop can service the
        // confirm.  In single-exec-thread mode the focused pane is almost
        // always the active one; in edge cases (focus change mid-turn) the
        // service_confirm runs on the new focused pane anyway.
        if (layout_ptr) layout_ptr->focused().editor.interrupt();
        return fut.get();
    });

    // ── Layout + initial pane ──────────────────────────────────────────────
    Rect full_rect{0, 0, arbiter::term_cols(), arbiter::term_rows()};
    LayoutTree layout(make_pane(), full_rect);
    layout_ptr = &layout;

    // Welcome card goes in the initial pane only.  Subsequent splits get a
    // clean start (no welcome), since splitting is an explicit user action.
    layout.focused().tui.draw_welcome(layout.focused().history);

    // Pump thread reads through g_getc_state.pane, which tracks the focused
    // pane for SIGWINCH repaints (output drain iterates all panes directly).
    g_getc_state.pane = &layout.focused();

    // Shared pane spawner — both the REPL's /pane command (handle() below)
    // and the orchestrator's pane_spawner callback (registered further
    // down) route through this function.  Forward-declared here so handle
    // can reference it; assigned after start_pane_thread exists, since
    // spawning starts an exec thread on the new pane.
    std::function<std::string(const std::string& agent, const std::string& message)>
        spawn_pane_fn;

    // ── Command handler ────────────────────────────────────────────────────
    // Takes the pane that owns the line; body references pane members via
    // per-call aliases so the existing code shape is preserved.
    auto handle = [&](Pane& pane, const std::string& line) {
        auto& tui             = pane.tui;
        auto& output_queue    = pane.output_queue;
        auto& thinking        = pane.thinking;
        auto& tool_indicator  = pane.tool_indicator;
        auto& current_agent   = pane.current_agent;
        auto& current_model   = pane.current_model;

        if (line[0] == '/') {
            // Parse command
            std::istringstream iss(line.substr(1));
            std::string cmd;
            iss >> cmd;

            if (cmd == "quit" || cmd == "exit" || cmd == "q") {
                quit_requested = true; return;
            }

            if (cmd == "agents") {
                std::string out;
                for (auto& id : orch.list_agents()) out += "  " + id + "\n";
                out += "\n";
                output_queue.push(out);
                return;
            }
            if (cmd == "status") {
                output_queue.push_msg(orch.global_status());
                return;
            }
            if (cmd == "tokens") {
                output_queue.push_msg("token stats: removed (billing is external)");
                return;
            }
            if (cmd == "use" || cmd == "switch") {
                std::string id;
                iss >> id;
                if (id == "index" || orch.has_agent(id)) {
                    current_agent = id;
                    current_model = orch.get_agent_model(id);
                    tui.update(current_agent, current_model, std::string(), agent_color(current_agent));
                } else {
                    output_queue.push_msg("ERR: no agent '" + id + "'");
                }
                return;
            }
            if (cmd == "send") {
                std::string id;
                iss >> id;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                try {
                    arbiter::MarkdownRenderer md;
                    if (!cfg.verbose) tool_indicator.begin();
                    StreamFilter filter(cfg,
                        [&md, &output_queue](const std::string& chunk) {
                            auto s = md.feed(chunk);
                            if (!s.empty()) output_queue.push(s);
                        });
                    auto resp = orch.send_streaming(id, msg,
                        [&filter](const std::string& chunk) { filter.feed(chunk); });
                    filter.flush();
                    auto tail = md.flush();
                    if (!tail.empty()) output_queue.push(tail);
                    auto tool_summary = tool_indicator.finalize();
                    if (!tool_summary.empty()) output_queue.push(tool_summary);
                    // Separator: md.flush() guarantees the stream ends with
                    // a `\n`, so one more gives exactly one blank line before
                    // the next message.
                    output_queue.end_message();
                    if (resp.ok) {
                        tui.update(current_agent, current_model, std::string(), agent_color(current_agent));
                        maybe_generate_title(msg, resp.content);
                    } else {
                        output_queue.push_msg(theme().accent_error + "ERR: " + resp.error + theme().reset + "");
                    }
                } catch (const std::exception& e) {
                    output_queue.push_msg(theme().accent_error + "ERR: " + std::string(e.what()) + theme().reset + "");
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
                    output_queue.push_msg(
                        "Usage: /pane <agent-id> <message>");
                    return;
                }
                std::string result = spawn_pane_fn
                    ? spawn_pane_fn(id, msg)
                    : std::string("ERR: pane spawner not initialized");
                output_queue.push_msg(result);
                return;
            }
            if (cmd == "ask") {
                std::string query;
                std::getline(iss, query);
                if (!query.empty() && query[0] == ' ') query.erase(0, 1);
                try {
                    arbiter::MarkdownRenderer md;
                    if (!cfg.verbose) tool_indicator.begin();
                    StreamFilter filter(cfg,
                        [&md, &output_queue](const std::string& chunk) {
                            auto s = md.feed(chunk);
                            if (!s.empty()) output_queue.push(s);
                        });
                    auto resp = orch.send_streaming("index", query,
                        [&filter](const std::string& chunk) { filter.feed(chunk); });
                    filter.flush();
                    auto tail = md.flush();
                    if (!tail.empty()) output_queue.push(tail);
                    auto tool_summary = tool_indicator.finalize();
                    if (!tool_summary.empty()) output_queue.push(tool_summary);
                    output_queue.end_message();
                    if (resp.ok) {
                        tui.update(current_agent, current_model, std::string(), agent_color(current_agent));
                        maybe_generate_title(query, resp.content);
                    } else {
                        output_queue.push_msg(theme().accent_error + "ERR: " + resp.error + theme().reset + "");
                    }
                } catch (const std::exception& e) {
                    output_queue.push_msg(theme().accent_error + "ERR: " + std::string(e.what()) + theme().reset + "");
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
                    output_queue.push_msg("Created: " + id + " (default config)\n"
                                      "Edit ~/.arbiter/agents/" + id + ".json to customize");
                } catch (const std::exception& e) {
                    output_queue.push_msg("ERR: " + std::string(e.what()));
                }
                return;
            }
            if (cmd == "remove") {
                std::string id;
                iss >> id;
                orch.remove_agent(id);
                output_queue.push_msg("Removed: " + id);
                if (current_agent == id) current_agent = "index";
                return;
            }
            if (cmd == "reset") {
                std::string id;
                iss >> id;
                if (id.empty()) id = current_agent;
                try {
                    orch.get_agent(id).reset_history();
                    output_queue.push_msg("History cleared: " + id);
                } catch (const std::exception& e) {
                    output_queue.push_msg("ERR: " + std::string(e.what()));
                }
                return;
            }
            if (cmd == "compact") {
                std::string id;
                iss >> id;
                if (id.empty()) id = current_agent;
                thinking.start("compacting");
                try {
                    std::string summary = orch.compact_agent(id);
                    thinking.stop();
                    if (summary.empty()) {
                        output_queue.push_msg("Nothing to compact: " + id + " has no history.");
                    } else {
                        output_queue.push_msg(
                            "\033[2m[compacted — context window cleared, summary held in session]" + theme().reset + "\n"
                            "\033[2m" + summary + theme().reset + "");
                    }
                } catch (const std::exception& e) {
                    thinking.stop();
                    output_queue.push_msg("ERR: " + std::string(e.what()));
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
                    output_queue.push_msg("Usage: /loop <agent> <initial prompt>");
                    return;
                }
                if (id != "index" && !orch.has_agent(id)) {
                    output_queue.push_msg("ERR: no agent '" + id + "'");
                    return;
                }
                std::string lid = loops.start(orch, id, prompt, &output_queue);
                output_queue.push_msg("Loop started: " + lid + " (agent: " + id + ")");
                return;
            }
            if (cmd == "loops") {
                output_queue.push_msg(loops.list());
                return;
            }
            if (cmd == "kill") {
                std::string lid;
                iss >> lid;
                if (loops.kill(lid))
                    output_queue.push_msg("Killed: " + lid);
                else
                    output_queue.push_msg("ERR: no loop '" + lid + "'");
                return;
            }
            if (cmd == "suspend") {
                std::string lid;
                iss >> lid;
                if (loops.suspend(lid))
                    output_queue.push_msg("Suspended: " + lid);
                else
                    output_queue.push_msg("ERR: no loop '" + lid + "' or not running");
                return;
            }
            if (cmd == "resume") {
                std::string lid;
                iss >> lid;
                if (loops.resume(lid))
                    output_queue.push_msg("Resumed: " + lid);
                else
                    output_queue.push_msg("ERR: no loop '" + lid + "' or not suspended");
                return;
            }
            if (cmd == "inject") {
                std::string lid;
                iss >> lid;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                if (loops.inject(lid, msg))
                    output_queue.push_msg("Injected into " + lid);
                else
                    output_queue.push_msg("ERR: no loop '" + lid + "'");
                return;
            }
            if (cmd == "log") {
                std::string lid;
                iss >> lid;
                if (lid.empty()) {
                    output_queue.push_msg("Usage: /log <loop-id> [last-N]");
                    return;
                }
                int n = 0;
                iss >> n;
                output_queue.push_msg(loops.log(lid, n));
                return;
            }
            if (cmd == "watch") {
                std::string lid;
                iss >> lid;
                if (lid.empty()) {
                    output_queue.push_msg("Usage: /watch <loop-id>");
                    return;
                }
                if (loops.is_stopped(lid) && loops.log_count(lid) == 0) {
                    output_queue.push_msg("ERR: no loop '" + lid + "'");
                    return;
                }
                // Dump everything buffered so far
                size_t seen = loops.log_count(lid);
                output_queue.push(loops.log(lid, 0));
                if (!loops.is_stopped(lid)) {
                    output_queue.push("\033[2m--- watching " + lid +
                                      " — press Enter to detach ---" + theme().reset + "\n");
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
                        output_queue.push_msg("\033[2m--- loop finished ---" + theme().reset + "");
                    } else {
                        output_queue.push_msg("\033[2m--- detached ---" + theme().reset + "");
                    }
                }
                return;
            }
            if (cmd == "fetch") {
                std::string url;
                iss >> url;
                if (url.empty()) {
                    output_queue.push_msg("Usage: /fetch <url>");
                    return;
                }
                thinking.start("fetching");
                std::string content = fetch_url(url);
                thinking.stop();
                if (content.substr(0, 4) == "ERR:") {
                    output_queue.push_msg(theme().accent_error + content + theme().reset + "");
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
                        output_queue.push_msg(arbiter::render_markdown(resp.content));
                        tui.update(current_agent, current_model, std::string(), agent_color(current_agent));
                    } else {
                        output_queue.push_msg(theme().accent_error + "ERR: " + resp.error + theme().reset + "");
                    }
                } catch (const std::exception& ex) {
                    thinking.stop();
                    output_queue.push_msg(theme().accent_error + "ERR: " + std::string(ex.what()) + theme().reset + "");
                }
                return;
            }
            if (cmd == "mem") {
                std::string subcmd;
                iss >> subcmd;
                if (subcmd == "shared") {
                    std::string action;
                    iss >> action;
                    if (action == "write") {
                        std::string text;
                        std::getline(iss, text);
                        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                        if (text.empty()) {
                            output_queue.push_msg("Usage: /mem shared write <text>");
                            return;
                        }
                        std::string wr = arbiter::cmd_mem_shared_write(text, get_memory_dir());
                        output_queue.push_msg(wr.substr(0, 3) == "ERR" ? wr : "Written to shared scratchpad");
                    } else if (action == "read" || action == "show") {
                        std::string mem = arbiter::cmd_mem_shared_read(get_memory_dir());
                        if (mem.empty())
                            output_queue.push_msg("Shared scratchpad is empty");
                        else
                            output_queue.push_msg(mem);
                    } else if (action == "clear") {
                        std::string cr = arbiter::cmd_mem_shared_clear(get_memory_dir());
                        output_queue.push_msg(cr.substr(0, 3) == "ERR" ? cr : "Shared scratchpad cleared");
                    } else {
                        output_queue.push_msg("Usage: /mem shared write <text> | /mem shared read | /mem shared clear");
                    }
                } else if (subcmd == "write") {
                    std::string text;
                    std::getline(iss, text);
                    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                    if (text.empty()) {
                        output_queue.push_msg("Usage: /mem write <text>");
                        return;
                    }
                    std::string result = write_memory(current_agent, text);
                    if (result.compare(0, 3, "ERR") == 0)
                        output_queue.push_msg(theme().accent_error + result + theme().reset + "");
                    else
                        output_queue.push_msg("Memory written for " + current_agent);
                } else if (subcmd == "read") {
                    std::string mem = read_memory(current_agent);
                    if (mem.empty()) {
                        output_queue.push_msg("No memory for " + current_agent);
                        return;
                    }
                    std::string msg = "[MEMORY for " + current_agent + "]:\n" +
                                      mem + "\n[END MEMORY]\n";
                    try {
                        thinking.start();
                        auto resp = orch.send(current_agent, msg);
                        thinking.stop();
                        if (resp.ok) {
                            output_queue.push_msg(arbiter::render_markdown(resp.content));
                            tui.update(current_agent, current_model, std::string(), agent_color(current_agent));
                        } else {
                            output_queue.push_msg("ERR: " + resp.error);
                        }
                    } catch (const std::exception& ex) {
                        thinking.stop();
                        output_queue.push_msg("ERR: " + std::string(ex.what()));
                    }
                } else if (subcmd == "show") {
                    std::string mem = read_memory(current_agent);
                    if (mem.empty())
                        output_queue.push_msg("No memory for " + current_agent);
                    else
                        output_queue.push_msg(mem);
                } else if (subcmd == "clear") {
                    std::string path = get_memory_dir() + "/" + current_agent + ".md";
                    fs::remove(path);
                    output_queue.push_msg("Memory cleared for " + current_agent);
                } else {
                    output_queue.push_msg("Usage: /mem write <text> | /mem read | /mem show | /mem clear\n"
                                      "       /mem shared write <text> | /mem shared read | /mem shared clear");
                }
                return;
            }
            if (cmd == "model") {
                std::string id, model;
                iss >> id >> model;
                if (id.empty() || model.empty()) {
                    output_queue.push_msg("Usage: /model <agent-id> <model-id>\n"
                                      "  e.g. /model research claude-haiku-4-5-20251001");
                    return;
                }
                try {
                    orch.get_agent(id).config_mut().model = model;
                    output_queue.push_msg(id + " model -> " + model);
                } catch (const std::exception& ex) {
                    output_queue.push_msg("ERR: " + std::string(ex.what()));
                }
                return;
            }
            if (cmd == "plan") {
                std::string subcmd;
                iss >> subcmd;
                if (subcmd != "execute") {
                    output_queue.push_msg("Usage: /plan execute <path>\n"
                                      "  Runs a plan file produced by /agent planner, executing each\n"
                                      "  phase sequentially and injecting prior outputs into dependents.");
                    return;
                }
                std::string path;
                iss >> path;
                if (path.empty()) {
                    output_queue.push_msg("Usage: /plan execute <path>");
                    return;
                }
                output_queue.push_msg("\033[2m[plan] executing: " + path + "]" + theme().reset + "");
                auto result = orch.execute_plan(path,
                    [&](const std::string& msg) {
                        output_queue.push_msg("\033[2m" + msg + theme().reset + "");
                    });
                if (!result.ok) {
                    output_queue.push_msg(theme().accent_error + "[plan] failed: " + result.error + theme().reset + "");
                } else {
                    output_queue.push_msg("\033[2m[plan] complete — " +
                                      std::to_string(result.phases.size()) + " phase(s) executed]" + theme().reset + "");
                    // Print final phase output (the deliverable)
                    if (!result.phases.empty()) {
                        auto& [num, name, out] = result.phases.back();
                        output_queue.push_msg(arbiter::render_markdown(out));
                    }
                }
                return;
            }
            if (cmd == "help") {
                output_queue.push_msg(
                    "Conversation\n"
                    "  <text>                           — send to the focused pane's agent\n"
                    "  /send <agent> <msg>              — send to a specific agent\n"
                    "  /ask <query>                     — ask the index master\n"
                    "  /use <agent>                     — switch the focused pane's current agent\n"
                    "\n"
                    "Agents\n"
                    "  /agents                          — list loaded agents\n"
                    "  /status                          — system status\n"
                    "  /tokens                          — full token + cost breakdown\n"
                    "  /create <id>                     — create agent with default config\n"
                    "  /remove <id>                     — remove agent\n"
                    "  /reset [id]                      — clear an agent's history (default: focused)\n"
                    "  /compact [id]                    — summarize + clear history (session memory)\n"
                    "  /model <agent> <model-id>        — change agent model at runtime\n"
                    "\n"
                    "Panes  (each pane is an independent conversation view)\n"
                    "  /pane <agent> <msg>              — spawn a parallel pane running the agent;\n"
                    "                                     result flows back to the spawner when done\n"
                    "  Ctrl-w v                         — split the focused pane vertically\n"
                    "  Ctrl-w s                         — split the focused pane horizontally\n"
                    "  Ctrl-w w                         — cycle focus to the next pane\n"
                    "  Ctrl-w c                         — close the focused pane\n"
                    "\n"
                    "Background loops\n"
                    "  /loop <agent> <prompt>           — run agent in a background loop\n"
                    "  /loops                           — list running / suspended loops\n"
                    "  /log <loop-id> [last-N]          — show buffered loop output\n"
                    "  /watch <loop-id>                 — tail loop output live (Enter to detach)\n"
                    "  /kill <loop-id>                  — stop a loop\n"
                    "  /suspend <loop-id>               — pause a loop\n"
                    "  /resume <loop-id>                — resume a paused loop\n"
                    "  /inject <loop-id> <msg>          — inject a message into a running loop\n"
                    "\n"
                    "Fetch + memory\n"
                    "  /fetch <url>                     — fetch URL, send readable text to agent\n"
                    "  /mem write <text>                — append note to agent's persistent memory\n"
                    "  /mem read                        — load agent memory into context\n"
                    "  /mem show                        — print raw memory file\n"
                    "  /mem clear                       — delete agent memory file\n"
                    "  /mem shared write <text>         — write to pipeline-shared scratchpad\n"
                    "  /mem shared read                 — read shared scratchpad\n"
                    "  /mem shared clear                — clear shared scratchpad\n"
                    "\n"
                    "Plans\n"
                    "  /plan execute <path>             — execute a planner-produced plan file\n"
                    "\n"
                    "Session\n"
                    "  /verbose [on|off]                — toggle raw /cmd line streaming (default off)\n"
                    "  /help                            — this list\n"
                    "  /quit                            — exit\n"
                    "\n"
                    "Scroll: PgUp / PgDn scroll the focused pane's history.  Esc cancels\n"
                    "any in-flight agent turn.");
                return;
            }
            if (cmd == "verbose") {
                std::string arg;
                iss >> arg;
                if (arg == "on")        cfg.verbose = true;
                else if (arg == "off")  cfg.verbose = false;
                else if (arg.empty())   cfg.verbose = !cfg.verbose;
                else {
                    output_queue.push_msg("Usage: /verbose [on|off]");
                    return;
                }
                output_queue.push_msg(std::string("verbose: ") +
                                      (cfg.verbose ? "on" : "off"));
                return;
            }

            output_queue.push_msg("Unknown command. /help for list.");
            return;
        }

        // Plain text → stream to current agent
        try {
            arbiter::MarkdownRenderer md;
            tool_indicator.begin();
            StreamFilter filter(cfg,
                [&md, &output_queue](const std::string& chunk) {
                    auto s = md.feed(chunk);
                    if (!s.empty()) output_queue.push(s);
                });
            auto resp = orch.send_streaming(current_agent, line,
                [&filter](const std::string& chunk) { filter.feed(chunk); });
            filter.flush();
            auto tail = md.flush();
            if (!tail.empty()) output_queue.push(tail);
            auto tool_summary = tool_indicator.finalize();
            if (!tool_summary.empty()) output_queue.push(tool_summary);
            // md.flush() guarantees the stream ended on `\n`; one more gives
            // exactly one blank line before the next message.
            output_queue.end_message();
            // Stash the raw agent response (or error) so start_pane_thread's
            // post-handle hook can flow it back to the parent when this is
            // a delegated pane.  Written regardless of resp.ok so the
            // parent sees the failure, not silence.
            pane.last_response = resp.ok ? resp.content
                                         : ("ERR: " + resp.error);
            if (resp.ok) {
                tui.update(current_agent, current_model, std::string(), agent_color(current_agent));
                maybe_generate_title(line, resp.content);
            } else {
                output_queue.push_msg(theme().accent_error + "ERR: " + resp.error + theme().reset + "");
            }
        } catch (const std::exception& e) {
            output_queue.push_msg(theme().accent_error + "ERR: " + std::string(e.what()) + theme().reset + "");
            pane.last_response = std::string("ERR: ") + e.what();
        }
        thinking.stop();
    };  // end handle lambda

    // Starts a pane's exec thread.  The thread pins g_active_pane at entry
    // (thread_local) so orch callbacks route to this pane, then drains the
    // pane's cmd_queue until it's stopped.  Caller is responsible for:
    //   (1) cmd_queue.stop() to unblock the blocking pop()
    //   (2) thread.join() before destroying the Pane
    auto start_pane_thread = [&](Pane& p_ref) {
        Pane* pane_ptr = &p_ref;
        pane_ptr->exec_thread = std::thread([&, pane_ptr]() {
            Pane& p = *pane_ptr;
            g_active_pane = &p;
            std::string line;
            while (p.cmd_queue.pop(line)) {
                p.cmd_queue.set_busy(true);
                handle(p, line);
                p.cmd_queue.set_busy(false);
                p.tui.clear_queue_indicator();

                // Delegation-chain flow-back: once per pane lifetime, after
                // the pane's first queued command completes (which is the
                // spawn_message for /pane-spawned children), push the
                // framed result into parent_pane->cmd_queue so the
                // delegating agent sees its sub-work's output as a fresh
                // message.  Then post a pending-close request so the REPL
                // can prompt the user before tearing this pane down.
                if (p.parent_pane != nullptr &&
                    !p.spawn_flowed.exchange(true)) {

                    // Verify the parent is still in the layout before
                    // touching its cmd_queue — a user Ctrl-w c or another
                    // chain-close could have removed it while we worked.
                    bool parent_alive = false;
                    Pane* parent = p.parent_pane;
                    {
                        std::lock_guard<std::recursive_mutex> lk(layout_mu);
                        layout.for_each_pane([&](Pane& other) {
                            if (&other == parent) parent_alive = true;
                        });
                    }

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

                    {
                        std::lock_guard<std::mutex> lk(pending_closes_mu);
                        pending_closes.push_back({&p, p.current_agent});
                    }
                    // Wake the main loop so it services the pending close
                    // promptly instead of waiting on the next keypress.
                    if (layout_ptr) layout_ptr->focused().editor.interrupt();
                }
            }
        });
    };

    // Dismiss any pane's welcome card.  Once the user has two or more panes
    // (user-split or agent-spawned), the greeting is noise — it clutters a
    // pane that the user is now trying to use as a work surface.  Each pane
    // still has its own scrollback; this just tears the card out of history
    // so render_scrollback no longer paints it.
    auto dismiss_welcome_everywhere = [&]() {
        layout.for_each_pane([&](Pane& p) {
            if (p.welcome_visible) {
                p.welcome_visible = false;
                p.history.clear();
                p.scroll_offset = 0;
                p.new_while_scrolled = 0;
                p.tui.clear_scroll_region();
            }
        });
    };

    // Pane spawner — called from an agent's exec thread when it emits
    // `/pane <agent> <msg>`.  Creates a new pane, starts its exec thread,
    // queues the message as the first command.  Returns a short status
    // string for the agent's tool-result block.  Fire-and-forget: the
    // spawning agent's context does NOT accumulate the new pane's output.
    //
    // Safety:
    //   - Takes layout_mu so the pump thread can't iterate mid-mutation.
    //   - Caps panes at kMaxPanes so a runaway agent can't keep splitting.
    //   - Refuses unknown agents and too-small focused panes.
    static constexpr size_t kMaxPanes = 8;
    spawn_pane_fn = [&](const std::string& req_agent,
                         const std::string& message) -> std::string {
        if (req_agent != "index" && !orch.has_agent(req_agent))
            return "ERR: no agent '" + req_agent + "'";

        std::lock_guard<std::recursive_mutex> lk(layout_mu);

        if (layout.pane_count() >= kMaxPanes) {
            return "ERR: pane cap reached (" + std::to_string(kMaxPanes) +
                   " open); close one before spawning more";
        }

        // The spawning pane is whichever pane's exec thread is currently
        // running (g_active_pane is set by start_pane_thread on entry).
        // Capture it so the new child can flow its result back here when
        // its task finishes.
        Pane* spawner_pane = g_active_pane;

        // Build the new pane via make_pane, then override its current_agent
        // to the requested sub-agent before the exec thread starts.  We
        // explicitly opt out of the focus transfer that split_focused does
        // by default — keeping focus on the spawner is important so the
        // user-facing readline cursor stays put, close prompts still
        // appear where they're reading, and multiple /pane spawns in the
        // same turn don't whip focus around three times.
        std::string captured_agent = req_agent;
        Pane* new_pane_ptr = layout.split_focused(
            LayoutTree::Orient::Vertical,
            [&, captured_agent]() -> std::unique_ptr<Pane> {
                auto p = make_pane();
                if (!p) return p;
                p->current_agent = captured_agent;
                p->current_model = orch.get_agent_model(captured_agent);
                p->tui.update(p->current_agent, p->current_model, "",
                              agent_color(p->current_agent));
                // No welcome card on agent-spawned panes — the first line
                // is the queued message, which matters more than greetings.
                p->welcome_visible = false;
                p->history.clear();
                return p;
            },
            /*focus_new=*/false);

        if (!new_pane_ptr) {
            return "ERR: focused pane too small to split";
        }

        Pane& new_pane = *new_pane_ptr;
        // Record delegation lineage so the child pane's exec thread can
        // flow its output back to the spawner when its task is done.
        new_pane.parent_pane   = spawner_pane;
        new_pane.spawn_message = message;
        new_pane.spawn_flowed.store(false);
        start_pane_thread(new_pane);
        new_pane.cmd_queue.push(message);

        // Tear the welcome card off any pane still showing it — if an
        // agent is spawning parallel work, the user is done with greetings.
        dismiss_welcome_everywhere();

        // Repaint every pane's scrollback so the relayout is coherent.
        layout.for_each_pane([&](Pane& p) {
            p.history.set_cols(p.tui.cols());
            p.tui.render_scrollback(p.history, p.scroll_offset,
                                     p.new_while_scrolled);
        });

        // Kick the main thread so its blocked read_line returns and the
        // REPL loop re-enters begin_input, which repaints the focused
        // pane's prompt over the input row that set_rect just blanked.
        refresh_focused_input.store(true);
        layout.focused().editor.interrupt();

        return "OK: spawned pane on agent '" + captured_agent +
               "'; output streams in its own view";
    };

    // Register the orchestrator callback — thin wrapper around
    // spawn_pane_fn so agent-emitted /pane lines and REPL-typed /pane
    // commands go through the same path.
    orch.set_pane_spawner([&](const std::string& agent, const std::string& msg) {
        return spawn_pane_fn(agent, msg);
    });

    // ── Per-pane exec threads ──────────────────────────────────────────────
    // Start the initial pane's thread now that handle is defined.  Every
    // pane created afterward (via dispatch_chord's split) starts its own
    // thread immediately.  Threads run in parallel: concurrent sends to
    // different agents execute simultaneously (same-provider sends still
    // serialize at ApiClient's connection mutex, which is a network-layer
    // constraint rather than an app-level one).
    start_pane_thread(layout.focused());

    // ── Output pump ────────────────────────────────────────────────────────
    // Drains every pane's output_queue every tick and repaints its scroll
    // region.  Holds layout_mu for the whole iteration so a concurrent
    // split/close/focus on the main thread can't mutate the tree mid-walk.
    std::atomic<bool> pump_stop{false};
    std::thread output_pump([&]() {
        auto flush_pane = [&](Pane& p) {
            std::string pending = p.output_queue.drain();
            if (pending.empty()) return;
            int before = (p.scroll_offset > 0) ? p.history.total_visual_rows() : 0;
            p.history.push(pending);
            if (p.scroll_offset > 0) {
                int after = p.history.total_visual_rows();
                p.new_while_scrolled += (after - before);
            }
            p.tui.render_scrollback(p.history, p.scroll_offset,
                                     p.new_while_scrolled);
        };
        while (!pump_stop.load()) {
            std::unique_lock<std::recursive_mutex> lk(layout_mu);
            if (g_winch) {
                g_winch = 0;
                Rect r{0, 0, arbiter::term_cols(), arbiter::term_rows()};
                layout.resize(r);
                layout.for_each_pane([&](Pane& p) {
                    p.history.set_cols(p.tui.cols());
                    p.tui.render_scrollback(p.history, p.scroll_offset,
                                             p.new_while_scrolled);
                });
                g_getc_state.pane = &layout.focused();
                // resize() blanked every pane's input row; kick the main
                // thread to repaint the focused pane's live prompt.
                refresh_focused_input.store(true);
                layout.focused().editor.interrupt();
            }
            layout.for_each_pane(flush_pane);
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        // Final drain — no need to lock here; exec threads have been joined
        // by this point (shutdown ordering), so no other thread is mutating.
        layout.for_each_pane(flush_pane);
    });

    // Service a pending confirm (destructive-action dialog from orch).  The
    // confirm lands on whichever pane currently has focus — that's where
    // the editor.interrupt() wakes the main loop.
    auto service_confirm = [&]() -> bool {
        std::promise<bool>* pending = nullptr;
        std::string         conf_prompt;
        {
            std::lock_guard<std::mutex> lk(confirm_state.mu);
            pending = confirm_state.pending;
            conf_prompt = confirm_state.prompt;
            confirm_state.pending = nullptr;
        }
        if (!pending) return false;

        Pane& pane = layout.focused();
        std::string rendered =
            "\n" + theme().accent_prompt + conf_prompt + " [y/N] " + theme().reset + "";
        pane.history.push(rendered);
        pane.tui.render_scrollback(pane.history, pane.scroll_offset,
                                    pane.new_while_scrolled);

        unsigned char ch = 0;
        ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        bool yes = (n == 1) && (ch == 'y' || ch == 'Y');
        std::string answer = yes
            ? std::string("\n" + theme().accent_success + "[user accepted input]" + theme().reset + "\n")
            : std::string("\n" + theme().accent_error + "[user denied input]" + theme().reset + "\n");
        pane.history.push(answer);
        pane.tui.render_scrollback(pane.history, pane.scroll_offset,
                                    pane.new_while_scrolled);
        pending->set_value(yes);
        return true;
    };

    // Service any pending-close requests queued by pane exec threads that
    // finished their delegated task.  Runs on the main thread; prompts the
    // user on the focused pane ("close X? [y/N]") and — on yes — stops the
    // target pane's cmd_queue, joins its thread, and removes it from the
    // layout.  Returns true if at least one entry was handled so the REPL
    // loop can re-enter read_line afterward.
    auto service_pending_closes = [&]() -> bool {
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
                layout.for_each_pane([&](Pane& p) {
                    if (&p == pc.pane) still_alive = true;
                });
            }
            if (!still_alive) continue;

            // Render the confirm prompt in the focused pane's scrollback.
            Pane& shown = layout.focused();
            std::string prompt =
                "\n" + theme().accent_prompt + "pane '" + pc.agent_id +
                "' finished — close it? [y/N] " + theme().reset + "";
            shown.history.push(prompt);
            shown.tui.render_scrollback(shown.history, shown.scroll_offset,
                                         shown.new_while_scrolled);

            unsigned char ch = 0;
            ssize_t n = ::read(STDIN_FILENO, &ch, 1);
            bool yes = (n == 1) && (ch == 'y' || ch == 'Y');

            std::string answer = yes
                ? std::string("\n" + theme().accent_success + "[closing '" + pc.agent_id + "']" + theme().reset + "\n")
                : std::string("\n" + theme().accent_error + "[keeping '" + pc.agent_id + "' open]" + theme().reset + "\n");
            shown.history.push(answer);
            shown.tui.render_scrollback(shown.history, shown.scroll_offset,
                                         shown.new_while_scrolled);

            if (yes) {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                layout.close_pane(pc.pane, [&](Pane& p) {
                    p.cmd_queue.stop();
                    if (p.exec_thread.joinable()) p.exec_thread.join();
                });
                g_getc_state.pane = &layout.focused();
                layout.for_each_pane([&](Pane& p) {
                    p.history.set_cols(p.tui.cols());
                    p.tui.render_scrollback(p.history, p.scroll_offset,
                                             p.new_while_scrolled);
                });
            }
        }
        return true;
    };

    // Chord dispatcher — runs after the focused editor returned with a
    // pending chord.  Takes layout_mu so the pump thread's iteration can't
    // observe a partially-mutated tree.  On close, we stop the victim pane's
    // cmd_queue and join its exec thread BEFORE the Pane is destroyed;
    // join() can block for however long the in-flight handle() takes (the
    // agent turn finishes or orch.cancel() aborts it).
    auto dispatch_chord = [&](char cmd) {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        bool spawned = false;
        switch (cmd) {
            case 'w':
            case 0x17:  // Ctrl-w Ctrl-w → next pane
                layout.focus_next();
                break;
            case 's':
                if (Pane* np = layout.split_focused(
                        LayoutTree::Orient::Horizontal, make_pane)) {
                    start_pane_thread(*np);
                    spawned = true;
                }
                break;
            case 'v':
                if (Pane* np = layout.split_focused(
                        LayoutTree::Orient::Vertical, make_pane)) {
                    start_pane_thread(*np);
                    spawned = true;
                }
                break;
            case 'c':
                if (layout.pane_count() > 1) {
                    layout.close_focused([&](Pane& p) {
                        // Stop queue so pop() returns, join the exec thread,
                        // then let the Pane destructor run with a quiesced
                        // thread.  Any pending output is dropped — a pane
                        // being closed doesn't need one last render.
                        p.cmd_queue.stop();
                        if (p.exec_thread.joinable()) p.exec_thread.join();
                    });
                }
                break;
        }
        // Manual split dismisses the welcome too — user has committed to
        // multi-pane work, don't leave the greeting on the original pane.
        if (spawned) dismiss_welcome_everywhere();
        // resize() inside split/close already called render_borders and
        // set_rect on each pane; we repaint scrollback for every pane so
        // their content survives the relayout.
        layout.for_each_pane([&](Pane& p) {
            p.history.set_cols(p.tui.cols());
            p.tui.render_scrollback(p.history, p.scroll_offset,
                                     p.new_while_scrolled);
        });
        g_getc_state.pane = &layout.focused();
    };

    // ── Main readline loop ──────────────────────────────────────────────────
    while (!quit_requested) {
        while (service_confirm()) {}
        while (service_pending_closes()) {}

        Pane& focused = layout.focused();
        focused.tui.begin_input([&focused]() { return focused.cmd_queue.pending(); });

        std::string prompt = focused.multiline_accum.empty()
            ? focused.tui.build_prompt()
            : "\001" + theme().prompt_color + "\002…\001" + theme().reset + "\002 ";

        std::string line;
        if (!focused.editor.read_line(prompt, line)) {
            char chord;
            if (focused.editor.take_chord(chord)) {
                dispatch_chord(chord);
                continue;
            }
            if (service_confirm()) continue;
            if (service_pending_closes()) continue;
            // Layout mutation woke us up just to repaint the focused
            // pane's prompt — loop back so begin_input paints a fresh
            // one.  Without this, read_line returning false here would
            // be treated as EOF and we'd exit.
            if (refresh_focused_input.exchange(false)) continue;
            break;   // real EOF
        }
        if (quit_requested) break;
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
                layout.for_each_pane([&](Pane& p) { p.cmd_queue.drain(); });
                break;
            }
        }

        if (focused.welcome_visible) {
            focused.welcome_visible = false;
            focused.history.clear();
            focused.scroll_offset      = 0;
            focused.new_while_scrolled = 0;
            focused.tui.clear_scroll_region();
        }

        focused.output_queue.push_msg(
            theme().user_echo_arrow + "> " + theme().user_echo_text + line + theme().reset + "");

        bool was_busy = focused.cmd_queue.is_busy();
        focused.cmd_queue.push(line);
        if (was_busy) {
            focused.tui.set_status("queued (" +
                std::to_string(focused.cmd_queue.pending()) + " waiting)");
        }
    }

    // ── Shutdown ───────────────────────────────────────────────────────────
    // Stop every pane's queue, then join its exec thread.  Do all stops
    // BEFORE joins so panes unblock in parallel.  After that the pump is
    // the only producer left and we can shut it down too.
    {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        layout.for_each_pane([&](Pane& p) { p.cmd_queue.stop(); });
        layout.for_each_pane([&](Pane& p) {
            if (p.exec_thread.joinable()) p.exec_thread.join();
        });
    }

    pump_stop = true;
    output_pump.join();

    if (stdin_is_tty) ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_stdin_tm);

    // Merge every pane's editor history into a single disk file, dropping
    // duplicates while preserving last-seen order.
    {
        std::ofstream hf(get_config_dir() + "/history");
        std::vector<std::string> merged;
        std::set<std::string> seen;
        layout.for_each_pane([&](Pane& p) {
            for (auto& h : p.editor.history())
                if (seen.insert(h).second) merged.push_back(h);
        });
        for (auto& h : merged) hf << h << '\n';
    }
    orch.save_session(session_path);

    TUI::leave_alt_screen();
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            cmd_interactive();
            return 0;
        }

        std::string arg1 = argv[1];

        if (arg1 == "--init" || arg1 == "init") {
            // arbiter --init [--force]
            // Without --force, --init preserves existing agent JSON files
            // so accidental re-runs don't clobber a user's edits.
            bool force = false;
            for (int i = 2; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--force" || a == "-f") force = true;
                else {
                    std::cerr << "Unknown --init flag: " << a << "\n";
                    return 1;
                }
            }
            arbiter::cmd_init(force);
            return 0;
        }
        if (arg1 == "--api" || arg1 == "api") {
            // arbiter --api [--port N] [--bind ADDR] [--verbose]
            int port = 8080;
            std::string bind = "127.0.0.1";
            bool verbose = false;
            for (int i = 2; i < argc; ) {
                std::string k = argv[i];
                if (k == "--verbose" || k == "-v") {
                    verbose = true;
                    ++i;
                    continue;
                }
                if (i + 1 >= argc) {
                    std::cerr << "--api flag '" << k << "' requires a value\n";
                    return 1;
                }
                std::string v = argv[i + 1];
                if      (k == "--port") port = std::atoi(v.c_str());
                else if (k == "--bind") bind = v;
                else {
                    std::cerr << "Unknown --api flag: " << k << "\n";
                    return 1;
                }
                i += 2;
            }
            arbiter::cmd_api(port, bind, verbose);
            return 0;
        }
        if (arg1 == "--send" || arg1 == "send") {
            if (argc < 4) {
                std::cerr << "Usage: arbiter --send <agent_id> <message>\n";
                return 1;
            }
            std::string agent = argv[2];
            std::string msg;
            for (int i = 3; i < argc; ++i) {
                if (i > 3) msg += " ";
                msg += argv[i];
            }
            arbiter::cmd_oneshot(agent, msg);
            return 0;
        }
        // Tenant identity admin — `arbiter --api` uses the resulting
        // tenants.db for bearer-token auth.  Billing (caps, usage, the
        // ledger) lives in the external billing service when configured
        // via $ARBITER_BILLING_URL.
        if (arg1 == "--add-tenant") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --add-tenant <name>\n";
                return 1;
            }
            arbiter::cmd_add_tenant(argv[2]);
            return 0;
        }
        if (arg1 == "--list-tenants") {
            arbiter::cmd_list_tenants();
            return 0;
        }
        if (arg1 == "--disable-tenant") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --disable-tenant <id|name>\n";
                return 1;
            }
            arbiter::cmd_disable_tenant(argv[2]);
            return 0;
        }
        if (arg1 == "--enable-tenant") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --enable-tenant <id|name>\n";
                return 1;
            }
            arbiter::cmd_enable_tenant(argv[2]);
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h" || arg1 == "help") {
            std::cout << BANNER;
            std::cout <<
                "Usage:\n"
                "  arbiter                            Interactive REPL\n"
                "  arbiter --api [--port N] [--bind ADDR] [--verbose]\n"
                "                                     HTTP+SSE orchestration API (default 127.0.0.1:8080).\n"
                "                                     --verbose mirrors every SSE event (text deltas, tool calls,\n"
                "                                     thinking, etc.) to stderr.  Env: ARBITER_API_VERBOSE=1.\n"
                "  arbiter --send <agent> <msg>       One-shot message\n"
                "  arbiter --init [--force]           Initialize config + example agents\n"
                "                                     --force overwrites existing ~/.arbiter/agents/*.json files;\n"
                "                                     omit it to preserve user-edited agent definitions.\n"
                "  arbiter --help                     This help\n\n"
                "Tenants (for --api):\n"
                "  arbiter --add-tenant <name>        Provision a tenant + API key\n"
                "  arbiter --list-tenants             List tenants\n"
                "  arbiter --disable-tenant <id|name> Revoke a tenant's access\n"
                "  arbiter --enable-tenant  <id|name> Restore a tenant's access\n\n"
                "Environment:\n"
                "  ANTHROPIC_API_KEY                  Claude API key\n"
                "  OPENAI_API_KEY                     OpenAI API key\n"
                "  OLLAMA_HOST                        Ollama server URL (default http://localhost:11434)\n"
                "  ARBITER_BILLING_URL                Billing service base URL.  When set, requests\n"
                "                                     pre-flight against /v1/runtime/quota/check and\n"
                "                                     post-turn usage to /v1/runtime/usage/record.\n"
                "                                     Empty ⇒ no billing; requests use provider keys.\n\n"
                "Config: ~/.arbiter/\n"
                "  api_key                            Anthropic key file\n"
                "  openai_api_key                     OpenAI key file\n"
                "  tenants.db                         Tenant identity store (--api)\n"
                "  agents/*.json                      Agent constitutions\n";
            return 0;
        }

        std::cerr << "Unknown option: " << arg1 << ". Try --help\n";
        return 1;

    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 1;
    }
}
