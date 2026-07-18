// Usage:
//   arbiter                          — interactive REPL
//   arbiter --api [--port 8080]      — HTTP+SSE API server
//   arbiter --send <agent> <msg>     — one-shot message
//   arbiter --init                   — create config dir + example agents

#include "orchestrator.h"
#include "commands.h"
#include "constitution.h"
#include "readline_wrapper.h"
#include "markdown.h"
#include "stream_renderer.h"
#include "render_policy.h"
#include "cli_helpers.h"
#include "cli.h"
#include "api_server.h"
#include "tenant_store.h"
#include "scheduler.h"
#include "notification_bus.h"
#include "repl/queues.h"
#include "loop_manager.h"
#include "cli.h"
#include "tui/tui.h"
#include "tui/tui_design.h"

#include <filesystem>
#include "tui/stream_filter.h"
#include "repl/pane.h"
#include "repl/layout.h"
#include "repl/pane_history.h"
#include "theme.h"
#include "config.h"
#include "repl/repl_argv.h"
#include "repl/conversation_store.h"
#include "repl/conversation_titling.h"
#include "repl/transcript_replay.h"
#include "tui/opentui/session.h"
#include "tui/sidebar.h"
#include "tui/history_sidebar.h"
#include "tui/opentui/sidebar_frame.h"
#include "tui/opentui/history_sidebar_frame.h"
#include "tui/opentui/top_header_frame.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/mouse_hit.h"

#include <iostream>
#include <string>
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

using arbiter::TenantStore;
using arbiter::Tenant;
using arbiter::ensure_primary_tenant;
using arbiter::make_cli_api_options;
using arbiter::ApiServerOptions;
using arbiter::wire_orchestrator_tools;
using arbiter::NotificationBus;
using arbiter::Scheduler;
using arbiter::get_config_dir;
using arbiter::get_api_keys;
using arbiter::fetch_url;
using arbiter::theme;


// ─── Terminal / TUI ──────────────────────────────────────────────────────────

static volatile sig_atomic_t g_winch = 0;
static void sigwinch_handler(int) { g_winch = 1; }

// Set while SGR mouse tracking is active so fatal-signal handlers can emit
// DECRST sequences without touching non-async-signal-safe state.
static volatile sig_atomic_t g_mouse_tracking = 0;

// Best-effort tty reset for fatal signals. Only async-signal-safe calls.
static void emergency_tty_reset() {
    // Disable any-event / button / mouse tracking + SGR mouse; show cursor;
    // leave alternate screen. Order matches OpenTUI's disableMouse path.
    static const char kReset[] =
        "\033[?1003l\033[?1002l\033[?1000l\033[?1006l"
        "\033[?25h\033[?1049l";
    (void)::write(STDOUT_FILENO, kReset, sizeof(kReset) - 1);
}

static void fatal_signal_handler(int sig) {
    if (g_mouse_tracking) emergency_tty_reset();
    // Restore default disposition and re-raise so the process still exits
    // with the expected status / core-dump behavior.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// Read one key for y/N confirms, skipping SGR mouse reports so a click's
// trailing button-Up cannot cancel the prompt.
static int read_confirm_key() {
    while (true) {
        char csi = 0;
        std::string params;
        const int key = arbiter::read_history_sidebar_key(csi, params);
        if (key < 0) return key;
        if (key == 0x1B && (csi == 'M' || csi == 'm')
            && !params.empty() && params[0] == '<') {
            continue;
        }
        return key;
    }
}


using arbiter::TUI;
using arbiter::ThinkingIndicator;
using arbiter::ToolCallIndicator;
using arbiter::StreamFilter;
using arbiter::StyleId;
using arbiter::CommandQueue;
using arbiter::OutputQueue;
using arbiter::LoopManager;
using arbiter::Pane;
using arbiter::LayoutTree;
using arbiter::PaneFrameHooks;
using arbiter::Rect;
using arbiter::SidebarState;
using arbiter::SidebarLoopEntry;
using arbiter::ConversationStore;
using arbiter::HistorySidebarState;
using arbiter::HistorySidebarKey;
using arbiter::HistorySidebarSnapshot;
using arbiter::read_history_sidebar_key;

// State the output-pump thread needs.  Points into cmd_interactive()'s stack
// frame — lifetime is the REPL call.  Pane* is the only per-pane hook; the
// pump reads pane.output_queue / pane.history / pane.tui through it.
struct ReplGetcState {
    Pane*                     pane = nullptr;
};

static ReplGetcState g_getc_state;
static arbiter::UiContext* g_ui_ctx = nullptr;

static void wire_markdown_diff_sink(arbiter::MarkdownRenderer& md, OutputQueue& oq) {
    md.set_diff_sink([&oq](const std::string& patch) {
        if (!patch.empty()) oq.push_diff(patch);
    });
}

static arbiter::RenderPolicy master_stream_policy(const arbiter::Config& cfg) {
    return cfg.verbose ? arbiter::kVerbose : arbiter::kMasterStream;
}

// Set by each pane's exec thread at startup and left untouched thereafter.
// Orchestrator callbacks (progress/tool-status/agent-start) run
// synchronously from whichever exec thread invoked orch.send_streaming, so
// reading this thread-local gets the owning pane with zero synchronization.
// Threads other than pane-exec (main, pump) leave it nullptr.
thread_local Pane* g_active_pane = nullptr;

// Pin the advisor gate's original task across foreground turns until the gate
// approves termination; mirrors LoopManager's original_task pinning.
static void update_pane_original_task(Pane& pane,
                                      const std::string& user_line,
                                      const arbiter::ApiResponse& resp) {
    if (resp.ok && resp.gate_approved) {
        pane.original_task.clear();
    } else if (resp.ok && pane.original_task.empty()) {
        pane.original_task = user_line;
    }
}

// Drain any pending exec output into the pane scroll view and repaint.
static void getc_flush_output() {
    auto& S = g_getc_state;
    if (!S.pane) return;
    pane_history_drain_queue(*S.pane);
    if (g_ui_ctx) pane_history_render(*S.pane, *g_ui_ctx);
}


// ─────────────────────────────────────────────────────────────────────────────

static void cmd_interactive(bool exec_allowed_flag, std::string_view theme_override = {}) {
    std::string dir = get_config_dir();
    arbiter::load_tui_design(dir, theme_override);
    auto api_keys = get_api_keys();

    // ── Raw stdin ──────────────────────────────────────────────────────────
    // Must happen before Session::start() below: setupTerminal() sends
    // capability queries (OSC 10/11, DECRQM, DA/XTVERSION, CPR) and the
    // terminal's replies land on stdin. With ECHO still enabled (inherited
    // cooked mode), the kernel line discipline echoes those bytes to the
    // screen the instant they arrive — reading them later doesn't undo
    // that. Raw mode has to be in effect *before* the queries go out.
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

    arbiter::opentui::Session ot_session;
    arbiter::UiContext ui_ctx;
    ot_session.start(static_cast<std::uint32_t>(arbiter::term_cols()),
                     static_cast<std::uint32_t>(arbiter::term_rows()));
    // Button+drag+wheel tracking without any-event motion (?1003).
    if (arbiter::tui_design().layout.mouse) {
        ot_session.engine().set_mouse_enabled(true, /*enable_movement=*/false);
        g_mouse_tracking = 1;
    }
    if (stdin_is_tty) arbiter::drain_stdin_spurious(200);
    ui_ctx.session = &ot_session;
    g_ui_ctx = &ui_ctx;

    arbiter::Orchestrator orch(std::move(api_keys));
    orch.load_agents(dir + "/agents");

    // ── App-scope shared state ─────────────────────────────────────────────
    arbiter::Config cfg;
    cfg.exec_allowed = exec_allowed_flag;
    orch.set_exec_disabled(!cfg.exec_allowed);
    LoopManager loops;

    // Serializes layout tree mutations against the pump thread's iteration.
    // Recursive because dispatch_chord can call back into the tree.
    std::recursive_mutex layout_mu;

    struct ConfirmState {
        std::mutex            mu;
        std::string           prompt;
        std::promise<bool>*   pending = nullptr;
    } confirm_state;

    std::atomic<bool> quit_requested{false};

    ConversationStore conversation_store(dir);
    HistorySidebarState history_sidebar;
    history_sidebar.set_enabled(arbiter::tui_design().layout.show_history_sidebar, dir);

    // Signals the main thread to repaint the focused input after a layout mutation.
    std::atomic<bool> refresh_focused_input{false};

    // Pane close requests queued by exec threads for the main thread to process.
    struct PendingClose {
        Pane*       pane;
        std::string agent_id;  // for the prompt text
    };
    std::mutex                pending_closes_mu;
    std::vector<PendingClose> pending_closes;

    // Conversation switch/delete requests queued by /chat (which runs on a
    // pane's exec thread) for the main thread to process — the actual
    // switch touches `layout`/stdin-adjacent state that isn't safe to
    // drive from a background thread. Mirrors pending_closes above.
    struct PendingConversationOp {
        bool switch_op = false;   // vs delete_op
        bool create_new = false; // only meaningful when switch_op
        std::string target_id;   // empty + create_new=false + switch_op=true means "use sidebar selection"
        bool delete_op = false;
        bool hard_delete = false;
    };
    std::mutex                          pending_conv_mu;
    std::vector<PendingConversationOp>  pending_conv_ops;

    SidebarState sidebar;

    // Forward declaration — layout_ptr lets lambdas registered before layout
    // construction reach it safely (we set it once and never clear).
    LayoutTree* layout_ptr = nullptr;

    // Filled in after switch_conversation exists; make_pane editors call through.
    std::function<bool(const arbiter::opentui::MouseEvent&)> route_mouse;

    // Active separator drag (mouse button held on a split gutter). Uses a
    // path-based SeparatorRef so a mid-drag layout mutation cannot UAF.
    struct MouseDragState {
        bool active = false;
        LayoutTree::SeparatorRef sep{};
    } mouse_drag;

    // History-row activation is queued out of route_mouse so we never nest
    // switch_conversation's stdin confirm inside the mouse handler / while
    // holding layout_mu across a blocking read.
    struct PendingMouseSwitch {
        bool pending = false;
        bool create_new = false;
    } mouse_switch;

    auto clear_mouse_drag = [&]() { mouse_drag = {}; };

    auto layout_bounds = [&sidebar, &layout_ptr, &history_sidebar]() -> Rect {
        const int cols = arbiter::term_cols();
        const int rows = arbiter::term_rows();
        const int leading = HistorySidebarState::width_for_terminal(
            cols, history_sidebar.enabled());
        const int panes = layout_ptr ? static_cast<int>(layout_ptr->pane_count()) : 1;
        const int trailing = sidebar.effective_width(cols, panes);
        // Row 0 is reserved for the top header bar.
        return Rect{leading, 1, std::max(1, cols - leading - trailing), std::max(1, rows - 1)};
    };

    bool restored = conversation_store.load(conversation_store.active_id(), orch);
    if (restored) sidebar.mark_prompt_started();

    TenantStore tenants;
    tenants.open(dir + "/tenants.db");
    Tenant primary = ensure_primary_tenant(tenants);

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

    std::string session_meta_path =
        sessions_dir + "/" + cwd_session_key() + ".conv";
    int64_t conversation_id = 0;
    {
        std::ifstream mf(session_meta_path);
        if (mf) mf >> conversation_id;
        if (conversation_id <= 0 ||
            !tenants.get_conversation(primary.id, conversation_id)) {
            auto conv = tenants.create_conversation(
                primary.id, "TUI session", "index");
            conversation_id = conv.id;
            std::ofstream wf(session_meta_path);
            wf << conversation_id << '\n';
        }
    }

    ApiServerOptions api_opts =
        make_cli_api_options(dir, get_api_keys(), exec_allowed_flag);
    wire_orchestrator_tools(orch, api_opts, tenants, primary.id, conversation_id);

    NotificationBus notifications;
    Scheduler scheduler(&api_opts, &tenants, &notifications);
    scheduler.start();

    // Load input history into one live store shared by every pane's editor:
    // a command typed in any pane is instantly in every pane's Up-arrow /
    // Ctrl-R history.
    auto shared_history = std::make_shared<arbiter::opentui::SharedInputHistory>();
    {
        std::vector<std::string> loaded;
        std::ifstream hf(get_config_dir() + "/history");
        std::string line;
        while (std::getline(hf, line))
            if (!line.empty()) loaded.push_back(std::move(line));
        shared_history->replace(std::move(loaded));
    }

    std::cout.flush();

    signal(SIGWINCH, sigwinch_handler);
    // Best-effort mouse/alt-screen cleanup if we die without running
    // Engine::~Engine (SIGTERM/HUP; SIGINT is usually blocked via ~ISIG).
    signal(SIGTERM, fatal_signal_handler);
    signal(SIGHUP, fatal_signal_handler);
    signal(SIGINT, fatal_signal_handler);

    std::function<void()> pump_notify;

    // Ctrl-P command palette: one shared table of every REPL command with a
    // one-line hint.  Kept in sync with /help by review (both live in this
    // file); Enter inserts the name into the input buffer.
    const std::vector<arbiter::PaletteItem> palette_items = {
        {"/ask",          "ask the index master"},
        {"/send",         "send a message to a specific agent"},
        {"/use",          "switch the focused pane's current agent"},
        {"/agents",       "list loaded agents"},
        {"/status",       "orchestrator status"},
        {"/tokens",       "token usage report"},
        {"/create",       "create agent with default config"},
        {"/remove",       "remove agent"},
        {"/reset",        "clear an agent's history"},
        {"/model",        "change agent model at runtime"},
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
        {"/exec",         "shell command (confirm gate)"},
        {"/write",        "write a file"},
        {"/read",         "conversation artifacts"},
        {"/list",         "list conversation artifacts"},
        {"/mcp",          "MCP server registry"},
        {"/a2a",          "remote A2A agents"},
        {"/lesson",       "agent-scoped lessons"},
        {"/plan",         "execute a planner-produced plan file"},
        {"/theme",        "switch TUI color theme in-session"},
        {"/verbose",      "toggle raw /cmd line streaming"},
        {"/chat list",    "list conversations"},
        {"/chat new",     "start a new conversation"},
        {"/chat switch",  "switch conversation"},
        {"/chat search",  "find text across saved conversations"},
        {"/chat title",   "rename the active conversation"},
        {"/help",         "command reference"},
        {"/quit",         "exit"},
    };

    // ── Pane factory ───────────────────────────────────────────────────────
    auto make_pane = [&]() -> std::unique_ptr<Pane> {
        auto p = std::make_unique<Pane>();
        // Wire pump wakeup so any output push wakes the drain thread
        // immediately rather than waiting for the next 30ms tick.
        p->output_queue.set_notify_fn([&pump_notify](){
            if (pump_notify) pump_notify();
        });
        p->current_agent = "index";
        p->current_model = orch.get_agent_model(p->current_agent);
        pane_history_init(*p);

        p->editor.set_shared_history(shared_history);
        p->editor.set_max_history(1000);
        p->editor.set_palette_items(palette_items);
        p->editor.set_present_fn([&]() { if (pump_notify) pump_notify(); });

        p->editor.set_completion_provider(
            [&orch, &loops, &dir](const std::string& buf, const std::string& tok)
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
                                  "/create","/remove","/reset","/model",
                                  "/pane","/find",
                                  "/loop","/loops","/log","/watch",
                                  "/kill","/suspend","/resume","/inject",
                                  "/fetch","/mem","/search","/browse",
                                  "/todo","/schedule","/exec","/write",
                                  "/read","/list","/mcp","/a2a","/lesson",
                                  "/plan","/theme","/verbose","/chat","/quit","/help"});
                }
                if (cmd == "send" || cmd == "use" || cmd == "loop" || cmd == "model" ||
                    cmd == "reset" || cmd == "pane") {
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
        p->editor.set_scroll_handler([raw, &pump_notify, &orch](int direction, int step) {
            const int max_off = pane_history_max_scroll(*raw);
            if (direction < 0) {
                raw->scroll_offset = std::min(raw->scroll_offset + step, max_off);
                // Hit the top of currently-loaded scrollback: pull in the
                // next chunk of older transcript history, if any is behind
                // the gap marker (see replay_transcript/kReplayTailMessages).
                if (raw->scroll_offset >= max_off && raw->scroll && raw->scroll->has_gap()) {
                    const auto history = orch.get_agent_history("index");
                    arbiter::replay_load_previous_chunk(*raw, history);
                }
            } else {
                raw->scroll_offset = std::max(0, raw->scroll_offset - step);
                raw->new_while_scrolled = 0;
                if (raw->scroll_offset == 0) raw->tui.clear_status();
            }
            if (pump_notify) pump_notify();
        });
        p->editor.set_code_expand_handler([raw, &pump_notify]() {
            if (pane_history_toggle_code_block(*raw, raw->scroll_offset) && pump_notify) {
                pump_notify();
            }
        });
        p->editor.set_cancel_handler([raw, &orch]() {
            orch.cancel();
            raw->multiline_accum.clear();
            raw->output_queue.push_prose_msg("[interrupted]", StyleId::Error);
        });
        // Chord prefix: Ctrl-w.  Recognized follow-ups: w (next pane),
        // h (horizontal split), v (vertical split), s (sidebar toggle),
        // c (close pane); Ctrl-w itself is a synonym for 'w'.
        p->editor.set_chord_handler([](char cmd) -> bool {
            return cmd == 'w' || cmd == 'h' || cmd == 's' || cmd == 'v' || cmd == 'c'
                || cmd == 't' || cmd == 'b'
                || cmd == 0x17;
        });
        p->editor.set_mouse_handler([&route_mouse](const arbiter::opentui::MouseEvent& ev) {
            return route_mouse ? route_mouse(ev) : false;
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
        arbiter::StreamRenderer renderer(arbiter::kInterim, p->output_queue);
        renderer.feed(content);
        renderer.flush();
    });
    orch.set_tool_status_callback([&](const std::string& kind, bool ok) {
        Pane* p = g_active_pane;
        if (p) p->tool_indicator.bump(kind, ok);
        sidebar.record_tool(kind, ok);
    });
    orch.set_cost_callback([&](const std::string& agent_id,
                                 const std::string& model,
                                 const arbiter::ApiResponse& resp) {
        sidebar.record_turn(agent_id, model, resp);
    });
    orch.set_agent_start_callback([&](const std::string& agent_id) {
        Pane* p = g_active_pane;
        if (p) p->thinking.start(agent_id + ": thinking");
    });
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
        if (layout_ptr) layout_ptr->focused().editor.interrupt();
        return fut.get();
    });

    // ── Layout + initial pane ──────────────────────────────────────────────
    LayoutTree layout(make_pane(), layout_bounds());
    layout_ptr = &layout;
    layout.for_each_pane([&](Pane& p) {
        pane_history_set_cols(p, p.tui.cols());
    });

    auto sync_layout_to_terminal = [&]() -> bool {
        const Rect want = layout_bounds();
        const Rect have = layout.outer_bounds();
        if (want.x == have.x && want.y == have.y
            && want.w == have.w && want.h == have.h) {
            return false;
        }
        layout.resize(want);
        layout.for_each_pane([&](Pane& p) {
            pane_history_set_cols(p, p.tui.cols());
        });
        return true;
    };

    auto reveal_sidebar = [&]() {
        if (sidebar.session_started()) return;
        sidebar.mark_prompt_started();
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        sync_layout_to_terminal();
        refresh_focused_input.store(true);
        layout.focused().editor.interrupt();
    };

    auto refresh_chrome = [&]() {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        layout.for_each_pane([&](Pane& p) { pane_history_retheme(p); });
        ot_session.apply_design();
        if (ui_ctx.present_all) ui_ctx.present_all();
        ot_session.flush_display();
        refresh_focused_input.store(true);
        layout.focused().editor.interrupt();
    };

    PaneFrameHooks pane_hooks;
    pane_hooks.for_each_pane = [&](const std::function<void(Pane&)>& fn) {
        layout.for_each_pane(fn);
    };
    pane_hooks.draw_overlays = [&](OpenTuiHandle frame, int cols, int rows) {
        if (frame == 0 || cols <= 0 || rows <= 0) return;

        arbiter::opentui::draw_top_header(frame, cols);

        const Rect hb = HistorySidebarState::rect_for_terminal(
            cols, rows, history_sidebar.enabled());
        if (hb.w > 0) {
            history_sidebar.refresh_entries(conversation_store);
            HistorySidebarSnapshot hs = history_sidebar.snapshot();
            hs.active_id = conversation_store.active_id();
            const arbiter::TuiChromeSnapshot chrome = layout.focused().tui.chrome_snapshot();
            arbiter::opentui::draw_history_sidebar(
                frame, hs, hb, chrome.rect, chrome.input_rows);
        }

        if (layout.pane_count() > 1) layout.draw_borders(frame);

        const int panes = static_cast<int>(layout.pane_count());
        int sw = sidebar.effective_width(cols, panes);
        if (sw <= 0) return;

        int pane_x = layout.outer_bounds().x;
        int pane_w = layout.outer_bounds().w;
        int gap = cols - pane_x - pane_w;
        if (sw <= 0 || gap <= 0) return;

        const Rect sb = {pane_x + pane_w, 1, std::min(sw, gap), std::max(1, rows - 1)};
        Pane& focused = layout.focused();
        sidebar.set_focus_context(focused.current_agent,
                                  focused.current_model,
                                  focused.original_task);
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
        const arbiter::SidebarSnapshot snap = sidebar.snapshot();
        const arbiter::TuiChromeSnapshot chrome = focused.tui.chrome_snapshot();
        arbiter::opentui::draw_sidebar(frame, snap, sb, chrome.rect, chrome.input_rows);
    };
    auto present_unlocked = [&]() {
        pane_history_present(ui_ctx, pane_hooks);
    };
    ui_ctx.present_all = [&]() {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        if (sync_layout_to_terminal()) refresh_focused_input.store(true);
        present_unlocked();
    };

    present_unlocked();

    // Exec-capability warning — list any agents that can run shell commands.
    // Queued here so the pump thread renders it on its first tick.
    if (cfg.exec_allowed) {
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
            layout.focused().output_queue.push_msg(
                "\033[2m[exec enabled: " + names +
                " \xe2\x80\x94 shell commands will run as you]\033[0m");
        }
    }

    g_getc_state.pane = &layout.focused();
    ui_ctx.focused_pane = &layout.focused();

    std::function<std::string(const std::string& agent, const std::string& message)>
        spawn_pane_fn;

    // ── Command handler ────────────────────────────────────────────────────
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
                    output_queue.push_msg("Usage: /find <text>, then /find next|prev to cycle");
                    return;
                } else {
                    r = pane_history_find(pane, rest);
                }
                if (r.total == 0) {
                    tui.set_status("find \"" + pane.find_term + "\": no matches");
                } else {
                    tui.set_status("find \"" + pane.find_term + "\": "
                                   + std::to_string(r.hit) + "/" + std::to_string(r.total)
                                   + "  /find next|prev");
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
                output_queue.push_msg(sidebar.tokens_report());
                return;
            }
            if (cmd == "use" || cmd == "switch") {
                std::string id;
                iss >> id;
                if (id == "index" || orch.has_agent(id)) {
                    current_agent = id;
                    current_model = orch.get_agent_model(id);
                    pane.original_task.clear();
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
                reveal_sidebar();
                try {
                    if (!cfg.verbose) tool_indicator.begin();
                    arbiter::StreamRenderer renderer(master_stream_policy(cfg), output_queue);
                    auto resp = orch.send_streaming(id, msg, [&](const std::string& chunk) {
                        renderer.feed(chunk);
                    });
                    renderer.flush();
                    {
                        const int n = tool_indicator.total();
                        const int f = tool_indicator.failed();
                        tool_indicator.finalize();
                        if (n > 0) {
                            output_queue.push_prose(arbiter::tool_call_summary_lines(n, f));
                        }
                    }
                    // Separator: md.flush() guarantees the stream ends with
                    // a `\n`, so one more gives exactly one blank line before
                    // the next message.
                    output_queue.end_message();
                    if (!resp.ok) {
                        output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
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
                reveal_sidebar();
                try {
                    if (!cfg.verbose) tool_indicator.begin();
                    arbiter::StreamRenderer renderer(master_stream_policy(cfg), output_queue);
                    auto resp = orch.send_streaming("index", query, [&](const std::string& chunk) {
                        renderer.feed(chunk);
                    });
                    renderer.flush();
                    {
                        const int n = tool_indicator.total();
                        const int f = tool_indicator.failed();
                        tool_indicator.finalize();
                        if (n > 0) {
                            output_queue.push_prose(arbiter::tool_call_summary_lines(n, f));
                        }
                    }
                    output_queue.end_message();
                    if (!resp.ok) {
                        output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
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
                    } else {
                        output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
                    }
                } catch (const std::exception& ex) {
                    thinking.stop();
                    output_queue.push_msg(theme().accent_error + "ERR: " + std::string(ex.what()) + theme().reset + "");
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
                std::string topic;
                std::getline(iss, topic);
                if (!topic.empty() && topic[0] == ' ') topic.erase(0, 1);
                if (!topic.empty()) {
                    std::string result = orch.execute_slash_command(line, current_agent);
                    output_queue.push_msg(result.empty()
                        ? "Unknown help topic '" + topic + "'"
                        : result);
                    return;
                }
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
                    "  /model <agent> <model-id>        — change agent model at runtime\n"
                    "\n"
                    "Panes  (each pane is an independent conversation view)\n"
                    "  /pane <agent> <msg>              — spawn a parallel pane running the agent;\n"
                    "                                     result flows back to the spawner when done\n"
                    "  Ctrl-w v                         — split the focused pane vertically\n"
                    "  Ctrl-w h                         — split the focused pane horizontally\n"
                    "  Ctrl-w s                         — toggle the session sidebar\n"
                    "  Ctrl-w w                         — cycle focus to the next pane\n"
                    "  Ctrl-w c                         — close the focused pane\n"
                    "  Ctrl-w t                         — toggle conversation history sidebar\n"
                    "  Ctrl-w b                         — enter sidebar to pick a conversation\n"
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
                    "  /mem write|read|show|clear       — scratchpad (same as API / agents)\n"
                    "  /mem shared write|read|clear     — tenant shared scratchpad\n"
                    "  /mem search|entries|entry|add    — structured memory graph\n"
                    "\n"
                    "Tools  (same dispatch as API agent turns)\n"
                    "  /search <query> [top=N]          — web search (Brave; needs API key)\n"
                    "  /browse <url>                    — fetch + extract readable text\n"
                    "  /todo list|add|start|done|…      — conversation-scoped task list\n"
                    "  /schedule list|<phrase>: <msg>   — schedule recurring/one-shot tasks\n"
                    "  /schedule cancel|pause|resume    — manage scheduled tasks by id\n"
                    "  /exec <cmd>                      — shell (confirm gate; off by default)\n"
                    "  /write <path>                    — write file (confirm gate)\n"
                    "  /read <path> | /list             — conversation artifacts\n"
                    "  /mcp tools|call                  — MCP server registry\n"
                    "  /a2a list|call                   — remote A2A agents\n"
                    "  /lesson list|add                 — agent-scoped lessons\n"
                    "  /help <topic>                    — detailed reference for one command\n"
                    "\n"
                    "Plans\n"
                    "  /plan execute <path>             — execute a planner-produced plan file\n"
                    "\n"
                    "Session\n"
                    "  /theme [list]|<preset>           — switch TUI color theme in-session\n"
                    "  /verbose [on|off]                — toggle raw /cmd line streaming (default off)\n"
                    "  /chat title <text>               — rename the active conversation (locks title)\n"
                    "  /chat search <text>              — find text across saved conversations\n"
                    "  /find <text> | next | prev       — search the focused pane's scrollback\n"
                    "  /help                            — this list\n"
                    "  /quit                            — exit\n"
                    "\n"
                    "Scroll: PgUp / PgDn scroll the focused pane's history.  Esc cancels\n"
                    "any in-flight agent turn.\n"
                    "Keys: Ctrl-P command palette, Ctrl-R reverse history search,\n"
                    "Ctrl-W pane chords (w/h/v/c/t/b).");
                return;
            }
            if (cmd == "chat") {
                std::string sub;
                iss >> sub;

                // Resolves "<n>" (1-based index into /chat list's order) or
                // an id-prefix to a real conversation id. Empty on no match.
                auto resolve_chat_target = [&](const std::string& arg) -> std::string {
                    if (arg.empty()) return {};
                    const auto entries = conversation_store.list();
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
                    const auto entries = conversation_store.list();
                    if (entries.empty()) {
                        output_queue.push_msg("(no conversations)");
                        return;
                    }
                    const std::string active = conversation_store.active_id();
                    std::ostringstream out;
                    int n = 1;
                    for (const auto& e : entries) {
                        out << (e.id == active ? "* " : "  ") << n << ". "
                            << (e.title.empty() ? "Untitled" : e.title)
                            << "  [" << e.id.substr(0, std::min<size_t>(8, e.id.size())) << "]\n";
                        ++n;
                    }
                    output_queue.push_msg(out.str());
                    return;
                }
                if (sub == "new") {
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({true, true, "", false, false});
                    }
                    if (layout_ptr) layout_ptr->focused().editor.interrupt();
                    output_queue.push_msg("switching to a new conversation...");
                    return;
                }
                if (sub == "switch") {
                    std::string arg;
                    iss >> arg;
                    const std::string id = resolve_chat_target(arg);
                    if (id.empty()) {
                        output_queue.push_msg("Usage: /chat switch <n | id-prefix> (see /chat list)");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({true, false, id, false, false});
                    }
                    if (layout_ptr) layout_ptr->focused().editor.interrupt();
                    output_queue.push_msg("switching...");
                    return;
                }
                if (sub == "title") {
                    std::string text;
                    std::getline(iss, text);
                    size_t a = 0;
                    while (a < text.size() && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
                    text = text.substr(a);
                    if (text.empty()) {
                        output_queue.push_msg("Usage: /chat title <text>");
                        return;
                    }
                    conversation_store.set_title_locked(conversation_store.active_id(), text);
                    output_queue.push_msg("title: " + text);
                    return;
                }
                if (sub == "delete") {
                    std::string arg;
                    iss >> arg;
                    const std::string id = resolve_chat_target(arg);
                    if (id.empty()) {
                        output_queue.push_msg("Usage: /chat delete <n | id-prefix> (see /chat list)");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({false, false, id, true, false});
                    }
                    if (layout_ptr) layout_ptr->focused().editor.interrupt();
                    output_queue.push_msg("deleted (session file kept — /chat purge removes it)");
                    return;
                }
                if (sub == "purge") {
                    std::string arg;
                    iss >> arg;
                    const std::string id = resolve_chat_target(arg);
                    if (id.empty()) {
                        output_queue.push_msg("Usage: /chat purge <n | id-prefix> (see /chat list)");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pending_conv_mu);
                        pending_conv_ops.push_back({false, false, id, true, true});
                    }
                    if (layout_ptr) layout_ptr->focused().editor.interrupt();
                    output_queue.push_msg("purged");
                    return;
                }
                if (sub == "search") {
                    std::string term;
                    std::getline(iss, term);
                    size_t a = 0;
                    while (a < term.size() && std::isspace(static_cast<unsigned char>(term[a]))) ++a;
                    term = term.substr(a);
                    if (term.empty()) {
                        output_queue.push_msg("Usage: /chat search <text>");
                        return;
                    }
                    // Flush the coalesced autosave first so the active
                    // conversation's newest turns are searchable too.
                    conversation_store.flush();
                    const auto hits = conversation_store.search(term);
                    if (hits.empty()) {
                        output_queue.push_msg("(no conversations match \"" + term + "\")");
                        return;
                    }
                    const std::string active = conversation_store.active_id();
                    std::ostringstream out;
                    for (const auto& h : hits) {
                        out << (h.id == active ? "* " : "  ")
                            << (h.title.empty() ? "Untitled" : h.title)
                            << "  [" << h.id.substr(0, std::min<size_t>(8, h.id.size())) << "]"
                            << "  (" << h.match_count
                            << (h.match_count == 1 ? " match)" : " matches)") << "\n";
                        if (!h.snippet.empty()) out << "      " << h.snippet << "\n";
                    }
                    out << "  Switch with /chat switch <id-prefix>.\n";
                    output_queue.push_msg(out.str());
                    return;
                }
                output_queue.push_msg("Usage: /chat list|new|switch|search|title|delete|purge");
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
                if (cmd == "theme") {
                std::string arg;
                iss >> arg;
                if (arg.empty() || arg == "list") {
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
                           "  /theme <preset>           — built-in or ~/.arbiter/themes/<name>.json\n"
                           "  /theme save <name>        — export current look to themes/<name>.json\n"
                           "  /theme file <path>        — load theme JSON (sets theme_file in tui.json)\n"
                           "\nConfig (~/.arbiter/tui.json):\n"
                           "  { \"preset\": \"nord\" }   or   { \"theme_file\": \"themes/mine.json\" }\n"
                           "Override any token with bg/text/accent/border/content groups (#RRGGBB).\n"
                           "Export a starter: arbiter --export-theme onedark > ~/.arbiter/themes/mine.json";
                    output_queue.push_msg(out.str());
                    return;
                }
                if (arg == "save") {
                    std::string name;
                    iss >> name;
                    if (name.empty()) {
                        output_queue.push_msg("Usage: /theme save <name>");
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
                        output_queue.push_msg("ERR: could not write " + path);
                        return;
                    }
                    output_queue.push_msg("saved theme: " + path);
                    return;
                }
                if (arg == "file") {
                    std::string path_arg;
                    iss >> path_arg;
                    if (path_arg.empty()) {
                        output_queue.push_msg("Usage: /theme file <path>\n"
                                            "  Path is relative to ~/.arbiter/ unless absolute.");
                        return;
                    }
                    if (!arbiter::set_tui_theme_file(dir, path_arg)) {
                        output_queue.push_msg("ERR: could not load theme file '" + path_arg + "'");
                        return;
                    }
                    refresh_chrome();
                    output_queue.push_msg("theme file: " + path_arg);
                    return;
                }
                if (!arbiter::tui_theme_name_is_valid(dir, arg)) {
                    output_queue.push_msg("ERR: unknown theme '" + arg + "' (/theme list)");
                    return;
                }
                arbiter::set_tui_preset(dir, arg);
                refresh_chrome();
                output_queue.push_msg("theme: " + arg);
                return;
            }

            {
                std::string result = orch.execute_slash_command(line, current_agent);
                if (!result.empty()) {
                    output_queue.push_msg(result);
                    return;
                }
            }

            output_queue.push_msg("Unknown command. /help for list.");
            return;
        }

        // Plain text → stream to current agent
        reveal_sidebar();
        try {
            tool_indicator.begin();
            arbiter::StreamRenderer renderer(master_stream_policy(cfg), output_queue);
            auto resp = orch.send_streaming(current_agent, line,
                [&](const std::string& chunk) { renderer.feed(chunk); },
                pane.original_task);
            renderer.flush();
            {
                const int n = tool_indicator.total();
                const int f = tool_indicator.failed();
                tool_indicator.finalize();
                if (n > 0) {
                    output_queue.push_prose(arbiter::tool_call_summary_lines(n, f));
                }
            }
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
            if (!resp.ok) {
                output_queue.push_prose_msg("ERR: " + resp.error, StyleId::Error);
            } else {
                // First turn of a still-"Untitled" conversation: set an
                // instant deterministic title from the user's message, then
                // best-effort refine it with a small model call in the
                // background. Both are no-ops once the conversation already
                // has a real (or locked) title.
                const std::string active_id = conversation_store.active_id();
                for (auto& e : conversation_store.list()) {
                    if (e.id != active_id || e.title != "Untitled") continue;
                    const std::string det = arbiter::deterministic_conversation_title(line);
                    if (!det.empty()) {
                        conversation_store.set_title(active_id, det);
                        std::string title_model = arbiter::load_title_model_override(dir);
                        if (title_model.empty()) title_model = orch.get_agent_model("index");
                        conversation_store.enqueue_title_job(active_id, line, resp.content,
                                                             title_model, orch);
                    }
                    break;
                }
            }
            // Durable per-turn autosave: coalesces onto the store's
            // background thread so a crash never loses more than the
            // in-flight turn, without stalling the input loop on JSON I/O.
            conversation_store.save_async(conversation_store.active_id(), orch);
        } catch (const std::exception& e) {
            output_queue.push_prose_msg("ERR: " + std::string(e.what()), StyleId::Error);
            pane.last_response = std::string("ERR: ") + e.what();
            conversation_store.save_async(conversation_store.active_id(), orch);
        }
        thinking.stop();
    };  // end handle lambda

    // Starts a pane's exec thread.  The thread pins g_active_pane at entry
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

                if (p.parent_pane != nullptr &&
                    !p.spawn_flowed.exchange(true)) {

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
                    if (layout_ptr) layout_ptr->focused().editor.interrupt();
                }
            }
        });
    };

    static constexpr size_t kMaxPanes = 8;
    spawn_pane_fn = [&](const std::string& req_agent,
                         const std::string& message) -> std::string {
        if (req_agent != "index" && !orch.has_agent(req_agent))
            return "ERR: no agent '" + req_agent + "'";

        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        clear_mouse_drag();

        if (layout.pane_count() >= kMaxPanes) {
            return "ERR: pane cap reached (" + std::to_string(kMaxPanes) +
                   " open); close one before spawning more";
        }

        Pane* spawner_pane = g_active_pane;
        std::string captured_agent = req_agent;
        Pane* new_pane_ptr = layout.split_focused(
            LayoutTree::Orient::Vertical,
            [&, captured_agent]() -> std::unique_ptr<Pane> {
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
        layout.for_each_pane([&](Pane& p) {
            pane_history_set_cols(p, p.tui.cols());
        });
        ui_ctx.present_all();

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
    // ── Output pump ────────────────────────────────────────────────────────
    // Drains every pane's output_queue every tick and repaints its scroll
    // region.  Holds layout_mu for the whole iteration so a concurrent
    // split/close/focus on the main thread can't mutate the tree mid-walk.
    //
    // The pump wakes immediately when any pane's OutputQueue receives data
    // (via the notify_fn_ callback) and falls back to a 30ms poll so
    // SIGWINCH repaints and the stop signal are still serviced promptly.
    std::mutex          pump_cv_mu;
    std::condition_variable pump_cv;
    bool                pump_notified = false;

    // Assign the notify function before starting any exec thread so the
    // callback is fully visible to any thread that calls push().
    pump_notify = [&]() {
        { std::lock_guard<std::mutex> lk(pump_cv_mu); pump_notified = true; }
        pump_cv.notify_one();
    };

    // Start exec thread after pump_notify is assigned — the exec thread
    // captures pump_notify by reference via OutputQueue::notify_fn_ and
    // may call push() on its first tick.
    start_pane_thread(layout.focused());

    std::atomic<bool> pump_stop{false};
    std::thread output_pump([&]() {
        auto push_pane_output = [&](Pane& p) { pane_history_drain_queue(p); };
        auto present_all = [&]() { present_unlocked(); };
        while (!pump_stop.load()) {
            {
                std::unique_lock<std::mutex> wlk(pump_cv_mu);
                pump_cv.wait_for(wlk, std::chrono::milliseconds(30),
                                 [&]{ return pump_notified || pump_stop.load(); });
                pump_notified = false;
            }
            std::unique_lock<std::recursive_mutex> lk(layout_mu);
            if (g_winch) g_winch = 0;
            if (sync_layout_to_terminal()) {
                g_getc_state.pane = &layout.focused();
                ui_ctx.focused_pane = &layout.focused();
                refresh_focused_input.store(true);
                layout.focused().editor.interrupt();
            }
            layout.for_each_pane(push_pane_output);
            present_all();
        }
        layout.for_each_pane(push_pane_output);
        present_all();
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
        pane_history_push(pane, rendered);
        ui_ctx.present_all();

        const int key = read_confirm_key();
        bool yes = (key == 'y' || key == 'Y');
        std::string answer = yes
            ? std::string("\n" + theme().accent_success + "[user accepted input]" + theme().reset + "\n")
            : std::string("\n" + theme().accent_error + "[user denied input]" + theme().reset + "\n");
        pane_history_push(pane, answer);
        ui_ctx.present_all();
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
            pane_history_push(shown, prompt);
            ui_ctx.present_all();

            const int key = read_confirm_key();
            bool yes = (key == 'y' || key == 'Y');

            std::string answer = yes
                ? std::string("\n" + theme().accent_success + "[closing '" + pc.agent_id + "']" + theme().reset + "\n")
                : std::string("\n" + theme().accent_error + "[keeping '" + pc.agent_id + "' open]" + theme().reset + "\n");
            pane_history_push(shown, answer);
            ui_ctx.present_all();

            if (yes) {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                clear_mouse_drag();
                layout.close_pane(pc.pane, [&](Pane& p) {
                    p.cmd_queue.stop();
                    if (p.exec_thread.joinable()) p.exec_thread.join();
                });
                g_getc_state.pane = &layout.focused();
                ui_ctx.focused_pane = &layout.focused();
                layout.for_each_pane([&](Pane& p) {
                    pane_history_set_cols(p, p.tui.cols());
                });
                present_unlocked();
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
        clear_mouse_drag();
        switch (cmd) {
            case 'w':
            case 0x17:  // Ctrl-w Ctrl-w → next pane
                layout.focus_next();
                break;
            case 's':
                if (sidebar.session_started()) {
                    sidebar.toggle_visible();
                    sync_layout_to_terminal();
                }
                break;
            case 'h':
                if (Pane* np = layout.split_focused(
                        LayoutTree::Orient::Horizontal, make_pane)) {
                    start_pane_thread(*np);
                }
                break;
            case 'v':
                if (Pane* np = layout.split_focused(
                        LayoutTree::Orient::Vertical, make_pane)) {
                    start_pane_thread(*np);
                }
                break;
            case 'c':
                if (layout.pane_count() > 1) {
                    layout.close_focused([&](Pane& p) {
                        p.cmd_queue.stop();
                        if (p.exec_thread.joinable()) p.exec_thread.join();
                    });
                }
                break;
            case 't':
                history_sidebar.toggle_enabled(dir);
                break;
            case 'b': {
                const int cols = arbiter::term_cols();
                if (HistorySidebarState::width_for_terminal(cols, true) <= 0) {
                    layout.focused().tui.set_status(
                        "History sidebar needs a wider terminal (>=72 cols) — try /chat list");
                    break;
                }
                if (!history_sidebar.enabled()) {
                    history_sidebar.set_enabled(true, dir);
                }
                history_sidebar.enter_focus(conversation_store,
                                            conversation_store.active_id());
                break;
            }
        }
        sync_layout_to_terminal();
        layout.for_each_pane([&](Pane& p) {
            pane_history_set_cols(p, p.tui.cols());
        });
        present_unlocked();
        g_getc_state.pane = &layout.focused();
        ui_ctx.focused_pane = &layout.focused();
    };

    auto any_turn_in_flight = [&]() {
        bool busy = false;
        layout.for_each_pane([&](Pane& p) { if (p.cmd_queue.is_busy()) busy = true; });
        return busy;
    };

    // Collapses to one pane, resets its state, and — if `replay` — restores
    // the now-active conversation's context and replays its transcript
    // tail. Shared by switch_conversation (below) and the "deleted the
    // active conversation" reassignment path. Caller must hold layout_mu,
    // have already cancelled/drained any in-flight turn, and (if `replay`)
    // have already pointed conversation_store's active id at the target
    // and called orch.reset_all_histories().
    auto apply_active_conversation = [&](bool replay) {
        clear_mouse_drag();
        if (replay) conversation_store.load(conversation_store.active_id(), orch);

        while (layout.pane_count() > 1) {
            layout.close_focused([&](Pane& p) {
                p.cmd_queue.stop();
                if (p.exec_thread.joinable()) p.exec_thread.join();
            });
        }

        Pane& focused = layout.focused();
        pane_history_clear(focused);
        focused.current_agent = "index";
        focused.current_model = orch.get_agent_model("index");
        focused.original_task.clear();
        focused.scroll_offset = 0;
        focused.new_while_scrolled = 0;
        focused.tui.clear_status();
        sync_layout_to_terminal();
        layout.for_each_pane([&](Pane& p) {
            pane_history_set_cols(p, p.tui.cols());
        });

        // Replay the tail of the restored conversation's transcript through
        // the same streaming pipeline live turns use, so the switch doesn't
        // leave the pane looking blank even though the agent's context was
        // restored. Older history stays behind a gap marker (PgUp loads it).
        if (replay) {
            const auto history = orch.get_agent_history("index");
            const size_t total = history.size();
            arbiter::replay_transcript(focused, history, arbiter::replay_tail_begin(total), total);
        }

        history_sidebar.refresh_entries(conversation_store);
        present_unlocked();
        g_getc_state.pane = &layout.focused();
        ui_ctx.focused_pane = &layout.focused();
    };

    // `explicit_id`: switch straight to this id, bypassing the sidebar's
    // current selection (used by /chat switch). Empty + !create_new reads
    // history_sidebar.selected_conversation_id() instead (sidebar Enter).
    auto switch_conversation = [&](bool create_new, std::string explicit_id = {}) {
        // Confirm outside layout_mu so the output pump can keep painting and
        // so nested mouse Up reports are drained by read_confirm_key.
        {
            bool busy = false;
            {
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                busy = any_turn_in_flight();
                if (busy) {
                    layout.focused().tui.set_status(
                        "Turn in progress — switch anyway? [y/N]");
                    present_unlocked();
                }
            }
            if (busy) {
                const int key = read_confirm_key();
                std::lock_guard<std::recursive_mutex> lk(layout_mu);
                layout.focused().tui.clear_status();
                if (key != 'y' && key != 'Y') {
                    history_sidebar.exit_focus();
                    present_unlocked();
                    return;
                }
            }
        }

        std::unique_lock<std::recursive_mutex> lk(layout_mu);
        clear_mouse_drag();

        orch.cancel();
        // Wait for every pane's exec thread to actually return to queue-pop
        // before touching orch's histories — cancel() only unblocks the
        // in-flight API call, it doesn't wait for the thread to unwind.
        // Drop the lock while spinning so the output pump can keep painting.
        while (any_turn_in_flight()) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lk.lock();
        }

        // Drain any in-flight autosave before we mutate orch's histories
        // below — otherwise a queued save from the previous turn could run
        // after reset_all_histories()/load_session() and clobber the old
        // conversation's file with the new one's (or empty) state.
        conversation_store.flush();
        conversation_store.save(conversation_store.active_id(), orch);

        if (create_new) {
            const std::string before = conversation_store.active_id();
            const std::string after = conversation_store.create_or_reuse(fs::current_path().string());
            if (after == before) {
                // Active conversation had no turns yet — reuse it instead of
                // leaving another empty "Untitled" entry behind.
                history_sidebar.exit_focus();
                present_unlocked();
                return;
            }
            orch.reset_all_histories();
            history_sidebar.exit_focus();
            apply_active_conversation(/*replay=*/false);
            return;
        }

        const std::string picked = !explicit_id.empty() ? explicit_id
                                                         : history_sidebar.selected_conversation_id();
        if (picked.empty() || picked == conversation_store.active_id()) {
            history_sidebar.exit_focus();
            present_unlocked();
            return;
        }
        conversation_store.set_active(picked);
        orch.reset_all_histories();
        history_sidebar.exit_focus();
        apply_active_conversation(/*replay=*/true);
    };

    // Soft/hard-deletes `id`. If it was the active conversation,
    // ConversationStore already reassigned active internally — bring the
    // pane in sync with whatever that new active conversation is.
    auto delete_conversation = [&](const std::string& id, bool hard) {
        std::lock_guard<std::recursive_mutex> lk(layout_mu);
        const bool was_active = (id == conversation_store.active_id());

        if (was_active) {
            orch.cancel();
            while (any_turn_in_flight()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            conversation_store.flush();
        }

        if (hard) conversation_store.purge(id);
        else conversation_store.soft_delete(id);

        if (was_active) {
            orch.reset_all_histories();
            apply_active_conversation(/*replay=*/true);
        } else {
            history_sidebar.refresh_entries(conversation_store);
            present_unlocked();
        }
    };

    // Drains /chat-queued switch/delete requests (see PendingConversationOp)
    // onto the main thread — /chat itself runs on a pane's exec thread and
    // must not drive `layout`/stdin directly.
    auto service_pending_conv_ops = [&]() -> bool {
        std::vector<PendingConversationOp> snapshot;
        {
            std::lock_guard<std::mutex> lk(pending_conv_mu);
            snapshot.swap(pending_conv_ops);
        }
        if (snapshot.empty()) return false;

        for (auto& op : snapshot) {
            if (op.switch_op) {
                switch_conversation(op.create_new, op.target_id);
            } else if (op.delete_op) {
                delete_conversation(op.target_id, op.hard_delete);
            }
        }
        return true;
    };

    auto scroll_pane = [&](Pane& pane, int direction, int step) {
        const int max_off = pane_history_max_scroll(pane);
        if (direction < 0) {
            pane.scroll_offset = std::min(pane.scroll_offset + step, max_off);
            if (pane.scroll_offset >= max_off && pane.scroll && pane.scroll->has_gap()) {
                const auto history = orch.get_agent_history("index");
                arbiter::replay_load_previous_chunk(pane, history);
            }
        } else {
            pane.scroll_offset = std::max(0, pane.scroll_offset - step);
            pane.new_while_scrolled = 0;
            if (pane.scroll_offset == 0) pane.tui.clear_status();
        }
        if (pump_notify) pump_notify();
    };

    auto right_sidebar_rect = [&]() -> Rect {
        const int cols = arbiter::term_cols();
        const int rows = arbiter::term_rows();
        const int panes = static_cast<int>(layout.pane_count());
        const int sw = sidebar.effective_width(cols, panes);
        if (sw <= 0) return {};
        const int pane_x = layout.outer_bounds().x;
        const int pane_w = layout.outer_bounds().w;
        const int gap = cols - pane_x - pane_w;
        if (gap <= 0) return {};
        return Rect{pane_x + pane_w, 1, std::min(sw, gap), std::max(1, rows - 1)};
    };

    auto history_visible_rows = [&](const Rect& hb) -> int {
        const arbiter::TuiChromeSnapshot chrome = layout.focused().tui.chrome_snapshot();
        return arbiter::opentui::history_sidebar_visible_rows(
            hb, chrome.rect, chrome.input_rows, history_sidebar.focused());
    };

    // Returns true when the focused editor should exit read_line (focus moved
    // or a pending history switch was queued).
    route_mouse = [&](const arbiter::opentui::MouseEvent& ev) -> bool {
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
                return false;
            }
            if (ev.type == MouseType::Drag && ev.button == MouseButton::Left) {
                if (!layout.drag_separator(mouse_drag.sep, ev.x, ev.y)) {
                    mouse_drag.active = false;
                } else {
                    layout.for_each_pane([&](Pane& p) {
                        pane_history_set_cols(p, p.tui.cols());
                    });
                    if (pump_notify) pump_notify();
                }
                return false;
            }
            if (ev.type == MouseType::Move) return false;
            // Any other event cancels an in-progress drag.
            mouse_drag.active = false;
        }

        const int cols = arbiter::term_cols();
        const int rows = arbiter::term_rows();
        const Rect hb = HistorySidebarState::rect_for_terminal(
            cols, rows, history_sidebar.enabled());
        const Rect rb = right_sidebar_rect();
        const int hist_vis = (hb.w > 0) ? history_visible_rows(hb) : 0;
        const auto hit = arbiter::opentui::hit_test(
            layout, hb, rb, history_sidebar.scroll_offset(), hist_vis,
            ev.x, ev.y);

        if (ev.type == MouseType::Scroll) {
            const int dir = (ev.scroll == ScrollDir::Up || ev.scroll == ScrollDir::Left)
                ? -1 : +1;
            if (hit.kind == HitKind::HistorySidebar && hb.w > 0) {
                history_sidebar.page_selection(dir, hist_vis);
                if (pump_notify) pump_notify();
                return false;
            }
            // Only scroll when the pointer is over a pane scroll/input/chrome
            // region — never fall back to the focused pane for Outside /
            // RightSidebar / separator hits.
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
                mouse_drag.active = true;
                mouse_drag.sep = hit.sep;
                return false;
            }
            if (hit.kind == HitKind::HistorySidebar) {
                if (!history_sidebar.focused()) {
                    history_sidebar.enter_focus(conversation_store,
                                                conversation_store.active_id());
                }
                if (hit.history_row >= 0) {
                    history_sidebar.select_at_index(hit.history_row, hist_vis);
                    // Queue activation — never call switch_conversation from
                    // inside the mouse handler (nested stdin confirm + lock).
                    mouse_switch.pending = true;
                    mouse_switch.create_new = history_sidebar.is_new_selected();
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
                const bool focus_changed = (hit.pane != layout.focused_ptr());
                const bool was_history = history_sidebar.focused();
                if (was_history) history_sidebar.exit_focus();
                layout.focus_pane(hit.pane);
                if (hit.kind == HitKind::PaneInput) {
                    hit.pane->editor.set_cursor_from_click(ev.x, ev.y);
                }
                if (focus_changed || was_history) {
                    refresh_focused_input.store(true);
                    if (pump_notify) pump_notify();
                    return true;
                }
                if (pump_notify) pump_notify();
                return false;
            }
        }

        return false;
    };

    auto service_mouse_switch = [&]() -> bool {
        if (!mouse_switch.pending) return false;
        const bool create_new = mouse_switch.create_new;
        mouse_switch.pending = false;
        switch_conversation(create_new);
        return true;
    };

    // ── Main readline loop ──────────────────────────────────────────────────
    while (!quit_requested) {
        while (service_confirm()) {}
        while (service_pending_closes()) {}
        while (service_pending_conv_ops()) {}
        while (service_mouse_switch()) {}

        if (history_sidebar.focused()) {
            if (pump_notify) pump_notify();
            const int cols = arbiter::term_cols();
            const int rows = arbiter::term_rows();
            const Rect hb = HistorySidebarState::rect_for_terminal(cols, rows, true);
            const arbiter::TuiChromeSnapshot chrome = layout.focused().tui.chrome_snapshot();
            const int visible_rows = arbiter::opentui::history_sidebar_visible_rows(
                hb, chrome.rect, chrome.input_rows, true);

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
                switch_conversation(history_sidebar.is_new_selected());
                continue;
            }
            if (action == HistorySidebarKey::New) {
                switch_conversation(true);
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
            if (action == HistorySidebarKey::RenameCommit) {
                const std::string id = history_sidebar.selected_conversation_id();
                const std::string text = history_sidebar.take_rename_buffer();
                if (!id.empty() && !text.empty()) {
                    conversation_store.set_title_locked(id, text);
                }
                history_sidebar.refresh_entries(conversation_store);
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::DeleteStart) {
                if (pump_notify) pump_notify();
                continue;
            }
            if (action == HistorySidebarKey::DeleteConfirmed) {
                const std::string id = history_sidebar.selected_conversation_id();
                if (!id.empty()) delete_conversation(id, /*hard=*/false);
                continue;
            }
            continue;
        }

        Pane& focused = layout.focused();
        ui_ctx.focused_pane = &focused;
        focused.tui.begin_input([&focused]() { return focused.cmd_queue.pending(); });

        std::string prompt = focused.multiline_accum.empty()
            ? focused.tui.build_prompt()
            : "\001" + theme().prompt_color + "\002"
                + arbiter::tui_design().component.continuation_prompt
                + "\001" + theme().reset + "\002";

        std::string line;
        if (!focused.editor.read_line(prompt, line)) {
            char chord;
            if (focused.editor.take_chord(chord)) {
                dispatch_chord(chord);
                continue;
            }
            if (service_confirm()) continue;
            if (service_pending_closes()) continue;
            if (service_pending_conv_ops()) continue;
            if (service_mouse_switch()) continue;
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

        focused.output_queue.push_msg(
            theme().user_echo_arrow + "> " + theme().user_echo_text + line + theme().reset + "");

        focused.cmd_queue.push(line);
        if (focused.cmd_queue.is_busy()) {
            focused.tui.show_queue_depth(focused.cmd_queue.pending());
            if (pump_notify) pump_notify();
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
    pump_cv.notify_one();   // unblock the pump's wait_for so it exits promptly
    output_pump.join();

    g_ui_ctx = nullptr;
    g_mouse_tracking = 0;
    ot_session.shutdown();

    // Discard any capability-query dribble while still in raw/no-echo mode.
    // This must run *before* tcsetattr restores ECHO below — bytes that
    // arrive after ECHO is back on get echoed to the screen by the kernel
    // the instant they're received, and reading them afterward doesn't
    // undo that.
    arbiter::drain_stdin_spurious(150);

    if (stdin_is_tty) ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_stdin_tm);

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
    // Drain any autosave still in flight, then do one last synchronous save
    // to capture anything that happened after the final turn's autosave.
    conversation_store.flush();
    conversation_store.save(conversation_store.active_id(), orch);

    scheduler.stop();

    ::write(STDOUT_FILENO, "\n", 1);
}

int main(int argc, char* argv[]) {
    try {
        if (argc >= 3 && std::string_view(argv[1]) == "--export-theme") {
            const std::string name = argv[2];
            if (!arbiter::tui_preset_is_valid(name)) {
                std::cerr << "Unknown preset: " << name << "\n";
                return 1;
            }
            std::cout << arbiter::tui_design_to_json(
                arbiter::tui_design_for_preset(name), "") << '\n';
            return 0;
        }

        if (arbiter::argv_has_theme_flag(argc, argv) &&
            arbiter::argv_repl_theme(argc, argv).empty()) {
            std::cerr << "Usage: arbiter [--no-exec] [--theme PRESET]\n";
            return 1;
        }

        if (arbiter::argv_launches_interactive(argc, argv)) {
            const std::string theme = arbiter::argv_repl_theme(argc, argv);
            const std::string dir = get_config_dir();
            if (!theme.empty() && !arbiter::tui_theme_name_is_valid(dir, theme)) {
                std::cerr << "Unknown theme: " << theme << "\n";
                std::cerr << "Built-in:";
                for (const auto& p : arbiter::tui_builtin_presets()) {
                    std::cerr << ' ' << p;
                }
                std::cerr << "\nCustom: place JSON in " << arbiter::tui_themes_dir(dir) << "/\n";
                return 1;
            }
            cmd_interactive(!arbiter::argv_has_no_exec(argc, argv), theme);
            return 0;
        }

        std::string arg1 = argv[1];

        if (arg1 == "--no-exec") {
            const std::string theme = arbiter::argv_repl_theme(argc, argv);
            const std::string dir = get_config_dir();
            if (!theme.empty() && !arbiter::tui_theme_name_is_valid(dir, theme)) {
                std::cerr << "Unknown theme: " << theme << "\n";
                return 1;
            }
            cmd_interactive(false, theme);
            return 0;
        }
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
            // arbiter --api [--port N] [--bind ADDR] [--verbose] [--allow-host-exec]
            int port = 8080;
            std::string bind = "127.0.0.1";
            bool verbose = false;
            bool allow_host_exec = false;
            for (int i = 2; i < argc; ) {
                std::string k = argv[i];
                if (k == "--verbose" || k == "-v") {
                    verbose = true;
                    ++i;
                    continue;
                }
                if (k == "--allow-host-exec") {
                    allow_host_exec = true;
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
            arbiter::cmd_api(port, bind, verbose, allow_host_exec);
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
        // tenants.db for bearer-token auth.
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
            std::cout <<
                "Usage:\n"
                "  arbiter [--theme PRESET]           Interactive REPL\n"
                "  arbiter --no-exec [--theme PRESET] Interactive REPL with /exec disabled\n"
                "                                     (agents cannot run shell commands)\n"
                "                                     --theme: onedark (default), modern, nord,\n"
                "                                     dracula, solarized, light, gruvbox,\n"
                "                                     catppuccin, tokyo-night, monokai, …\n"
                "                                     (/theme list for all presets)\n"
                "  arbiter --api [--port N] [--bind ADDR] [--verbose] [--allow-host-exec]\n"
                "                                     HTTP+SSE orchestration API (default 127.0.0.1:8080).\n"
                "                                     --verbose mirrors every SSE event (text deltas, tool calls,\n"
                "                                     thinking, etc.) to stderr.  Env: ARBITER_API_VERBOSE=1.\n"
                "                                     --allow-host-exec permits agents to run shell commands on\n"
                "                                     the host via popen().  WARNING: agents run as this process's\n"
                "                                     user.  Also: ARBITER_ALLOW_HOST_EXEC=1.\n"
                "  arbiter --send <agent> <msg>       One-shot message\n"
                "  arbiter --export-theme PRESET      Write full theme JSON to stdout\n"
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
                "  OPENROUTER_API_KEY                 OpenRouter key for hosted models\n"
                "  OLLAMA_HOST                        Ollama server URL (default http://localhost:11434)\n"
                "Config: ~/.arbiter/\n"
                "  openrouter_api_key                 OpenRouter key file\n"
                "  tui.json                           Theme preset, theme_file, or overrides\n"
                "  themes/*.json                      Custom theme documents\n"
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
