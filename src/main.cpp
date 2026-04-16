// Usage:
//   index                          — interactive REPL
//   index --serve [--port 9077]    — start TCP server
//   index --send <agent> <msg>     — one-shot message
//   index --init                   — generate token + example agents
//   index --gen-token              — generate new auth token

#include "orchestrator.h"
#include "commands.h"
#include "constitution.h"
#include "readline_wrapper.h"
#include "cost_tracker.h"
#include "markdown.h"
#include "cli_helpers.h"
#include "repl/queues.h"
#include "loop_manager.h"
#include "title_generator.h"
#include "cli.h"
#include "tui/tui.h"
#include "tui/line_editor.h"

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
#include <sstream>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>

namespace fs = std::filesystem;

using index_ai::BANNER;
using index_ai::agent_color;
using index_ai::get_config_dir;
using index_ai::get_memory_dir;
using index_ai::get_api_key;
using index_ai::write_memory;
using index_ai::read_memory;
using index_ai::fetch_url;


// ─── Terminal / TUI ──────────────────────────────────────────────────────────

static volatile sig_atomic_t g_winch = 0;
static void sigwinch_handler(int) { g_winch = 1; }


using index_ai::TUI;
using index_ai::ThinkingIndicator;
using index_ai::CommandQueue;
using index_ai::OutputQueue;
using index_ai::LoopManager;

struct ReplGetcState {
    OutputQueue*              output_queue       = nullptr;
    TUI*                      tui                = nullptr;
    index_ai::ScrollBuffer*   history            = nullptr;
    int*                      scroll_offset      = nullptr;   // in VISUAL rows
    int*                      new_while_scrolled = nullptr;   // in VISUAL rows
    std::atomic<bool>*        quit_requested     = nullptr;
    index_ai::Orchestrator*   orch               = nullptr;
    std::string*              multiline_accum    = nullptr;
};

static ReplGetcState g_getc_state;

static void fwrite_crlf(const std::string& s) {
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            if (i > start) fwrite(s.data() + start, 1, i - start, stdout);
            fputs("\r\n", stdout);
            start = i + 1;
        }
    }
    if (start < s.size()) fwrite(s.data() + start, 1, s.size() - start, stdout);
}

// Drain any pending exec output, record it in the scroll buffer, and render.
static void getc_flush_output() {
    auto& S = g_getc_state;
    if (!S.output_queue || !S.history || !S.tui) return;
    std::string pending = S.output_queue->drain();
    if (pending.empty()) return;

    int before = (*S.scroll_offset > 0) ? S.history->total_visual_rows() : 0;
    S.history->push(pending);

    std::lock_guard<std::recursive_mutex> lk(S.tui->tty_mutex());
    if (*S.scroll_offset > 0) {
        int after = S.history->total_visual_rows();
        *S.new_while_scrolled += (after - before);
        S.tui->render_scrollback(*S.history, *S.scroll_offset, *S.new_while_scrolled);
    } else {
        printf("\0337");
        printf("\033[%d;1H", S.tui->last_scroll_row());
        fwrite_crlf(pending);
        printf("\0338");
        fflush(stdout);
    }
}


// ─────────────────────────────────────────────────────────────────────────────

static void cmd_interactive() {
    std::string dir = get_config_dir();
    std::string api_key = get_api_key();

    index_ai::Orchestrator orch(api_key);
    orch.set_memory_dir(get_memory_dir());
    orch.load_agents(dir + "/agents");

    std::string current_agent = "index";
    std::string current_model = orch.get_agent_model(current_agent);

    // Init TUI before any output (clears screen, sets scroll region, draws header).
    TUI tui;
    tui.init(current_agent, current_model, agent_color(current_agent));

    // Session files are scoped to the working directory so that starting
    // index in different repos doesn't bleed history across contexts.
    // We hash the cwd to produce a short, filesystem-safe filename.
    auto cwd_session_key = []() -> std::string {
        std::string cwd = fs::current_path().string();
        // FNV-1a 32-bit hash
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

    std::cout.flush();

    signal(SIGWINCH, sigwinch_handler);

    // Output queue: exec and loop threads push here; main thread flushes
    // (with CRLF expansion) via getc_flush_output.  Message separation is
    // handled by the queue itself — callers use output_queue.push_msg() for single-call
    // messages or push() + end_message() for streamed messages; the queue
    // materialises exactly one blank-line separator between adjacent
    // messages so per-call strings never need trailing `\n\n`.
    OutputQueue output_queue;

    // sub-agent progress
    orch.set_progress_callback([&output_queue](const std::string& agent_id,
                                                const std::string& content) {
        const char* dim = "\033[38;5;238m";
        const char* rst = "\033[0m";
        std::string buf;
        std::istringstream ss(content);
        std::string ln;
        while (std::getline(ss, ln)) {
            buf += dim;
            buf += "  ";
            buf += ln;
            buf += rst;
            buf += "\n";
        }
        output_queue.push_msg(buf);
    });

    ThinkingIndicator thinking(&tui);
    LoopManager loops;
    index_ai::CostTracker tracker;

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

    // ── Line editor ────────────────────────────────────────────────────────
    index_ai::LineEditor editor(tui);

    // Record sub-agent token costs that bypass the top-level REPL handler.
    orch.set_cost_callback([&tracker](const std::string& agent_id,
                                       const std::string& model,
                                       const index_ai::ApiResponse& resp) {
        tracker.record(agent_id, model, resp);
    });

    orch.set_agent_start_callback([&thinking](const std::string& agent_id) {
        thinking.start(agent_id + ": thinking");
    });

    // Surface auto-compact events in the header instead of leaking to stderr.
    orch.set_compact_callback([&tui](const std::string& agent_id, size_t n) {
        tui.set_status(agent_id + ": compacting context (" +
                       std::to_string(n) + " msgs)");
    });

    // Duplicate tool calls are silently swallowed by execute_agent_commands'
    // dedup gate — no [dup] banner, no DUPLICATE block fed back to the model.
    // No callback registered here on purpose.

    // ── Confirm dialog for destructive agent actions ──────────────────────
    struct ConfirmState {
        std::mutex            mu;
        std::string           prompt;
        std::promise<bool>*   pending = nullptr;
    } confirm_state;
    orch.set_confirm_callback([&confirm_state, &editor](const std::string& p) -> bool {
        std::promise<bool> done;
        auto fut = done.get_future();
        {
            std::lock_guard<std::mutex> lk(confirm_state.mu);
            confirm_state.prompt  = p;
            confirm_state.pending = &done;
        }
        editor.interrupt();
        return fut.get();
    });

    editor.set_max_history(1000);
    {
        std::ifstream hf(get_config_dir() + "/history");
        std::vector<std::string> loaded;
        std::string line;
        while (std::getline(hf, line)) if (!line.empty()) loaded.push_back(std::move(line));
        editor.set_history(std::move(loaded));
    }

    // Tab completion: slash commands, agent names, loop IDs
    editor.set_completion_provider(
        [&orch, &loops](const std::string& buf, const std::string& tok)
        -> std::vector<std::string> {
            auto match = [&](const std::vector<std::string>& candidates) {
                std::vector<std::string> out;
                for (auto& c : candidates)
                    if (c.substr(0, tok.size()) == tok) out.push_back(c);
                return out;
            };

            // Extract first word from buffer (the command)
            std::string cmd;
            {
                std::istringstream iss(buf);
                iss >> cmd;
                if (!cmd.empty() && cmd[0] == '/') cmd = cmd.substr(1);
            }
            bool only_cmd = (buf.find(' ') == std::string::npos);

            // Completing the slash command itself
            if (only_cmd || buf.empty()) {
                return match({"/send","/ask","/use","/agents","/status","/tokens",
                              "/create","/remove","/reset","/compact","/model",
                              "/loop","/loops","/log","/watch",
                              "/kill","/suspend","/resume","/inject",
                              "/fetch","/mem","/plan","/quit","/help"});
            }

            // Agent name completion
            if (cmd == "send" || cmd == "use" || cmd == "loop" || cmd == "model" ||
                cmd == "reset" || cmd == "compact") {
                auto agents = orch.list_agents();
                agents.push_back("index");
                return match(agents);
            }

            // Loop ID completion
            if (cmd == "kill"    || cmd == "suspend" || cmd == "resume" ||
                cmd == "watch"   || cmd == "log"     || cmd == "inject") {
                return match(loops.list_ids());
            }

            // /mem subcommands
            if (cmd == "mem") {
                return match({"write","read","show","clear"});
            }

            return {};
        });

    // ── Command queue + execution thread ────────────────────────────────────────
    CommandQueue cmd_queue;
    std::atomic<bool> quit_requested{false};
    std::atomic<bool> title_generated{false};  // generate title only once per session

    // Helper: fire title generation after the first successful response.
    auto maybe_generate_title = [&](const std::string& user_msg,
                                     const std::string& response_snippet) {
        if (title_generated.exchange(true)) return;  // already done
        index_ai::generate_title_async(orch.client(), user_msg, response_snippet,
            [&tui](const std::string& t){ tui.set_title(t); });
    };

    // All command handling lives in this lambda, called from the exec thread.
    // ALL output goes through output_queue.push() — never directly to stdout.
    auto handle = [&](const std::string& line) {

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
                output_queue.push_msg(tracker.format_summary());
                return;
            }
            if (cmd == "use" || cmd == "switch") {
                std::string id;
                iss >> id;
                if (id == "index" || orch.has_agent(id)) {
                    current_agent = id;
                    current_model = orch.get_agent_model(id);
                    tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
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
                    index_ai::MarkdownRenderer md;
                    auto resp = orch.send_streaming(id, msg,
                        [&md, &output_queue](const std::string& chunk) {
                            auto s = md.feed(chunk);
                            if (!s.empty()) output_queue.push(s);
                        });
                    auto tail = md.flush();
                    if (!tail.empty()) output_queue.push(tail);
                    // Separator: md.flush() guarantees the stream ends with
                    // a `\n`, so one more gives exactly one blank line before
                    // the next message.
                    output_queue.end_message();
                    if (resp.ok) {
                        tracker.record(id, orch.get_agent_model(id), resp);
                        tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                        maybe_generate_title(msg, resp.content);
                    } else {
                        output_queue.push_msg("\033[38;5;167mERR: " + resp.error + "\033[0m");
                    }
                } catch (const std::exception& e) {
                    output_queue.push_msg("\033[38;5;167mERR: " + std::string(e.what()) + "\033[0m");
                }
                thinking.stop();
                return;
            }
            if (cmd == "ask") {
                std::string query;
                std::getline(iss, query);
                if (!query.empty() && query[0] == ' ') query.erase(0, 1);
                try {
                    index_ai::MarkdownRenderer md;
                    auto resp = orch.send_streaming("index", query,
                        [&md, &output_queue](const std::string& chunk) {
                            auto s = md.feed(chunk);
                            if (!s.empty()) output_queue.push(s);
                        });
                    auto tail = md.flush();
                    if (!tail.empty()) output_queue.push(tail);
                    output_queue.end_message();
                    if (resp.ok) {
                        tracker.record("index", orch.get_agent_model("index"), resp);
                        tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                        maybe_generate_title(query, resp.content);
                    } else {
                        output_queue.push_msg("\033[38;5;167mERR: " + resp.error + "\033[0m");
                    }
                } catch (const std::exception& e) {
                    output_queue.push_msg("\033[38;5;167mERR: " + std::string(e.what()) + "\033[0m");
                }
                thinking.stop();
                return;
            }
            if (cmd == "create") {
                std::string id;
                iss >> id;
                try {
                    auto config = index_ai::master_constitution();
                    config.name = id;
                    orch.create_agent(id, std::move(config));
                    output_queue.push_msg("Created: " + id + " (default config)\n"
                                      "Edit ~/.index/agents/" + id + ".json to customize");
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
                            "\033[2m[compacted — context window cleared, summary held in session]\033[0m\n"
                            "\033[2m" + summary + "\033[0m");
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
                std::string lid = loops.start(orch, id, prompt, &tracker, &output_queue);
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
                                      " — press Enter to detach ---\033[0m\n");
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
                        output_queue.push_msg("\033[2m--- loop finished ---\033[0m");
                    } else {
                        output_queue.push_msg("\033[2m--- detached ---\033[0m");
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
                    output_queue.push_msg("\033[38;5;167m" + content + "\033[0m");
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
                        output_queue.push_msg(index_ai::render_markdown(resp.content));
                        tracker.record(current_agent, orch.get_agent_model(current_agent), resp);
                        tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                    } else {
                        output_queue.push_msg("\033[38;5;167mERR: " + resp.error + "\033[0m");
                    }
                } catch (const std::exception& ex) {
                    thinking.stop();
                    output_queue.push_msg("\033[38;5;167mERR: " + std::string(ex.what()) + "\033[0m");
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
                        index_ai::cmd_mem_shared_write(text, get_memory_dir());
                        output_queue.push_msg("Written to shared scratchpad");
                    } else if (action == "read" || action == "show") {
                        std::string mem = index_ai::cmd_mem_shared_read(get_memory_dir());
                        if (mem.empty())
                            output_queue.push_msg("Shared scratchpad is empty");
                        else
                            output_queue.push_msg(mem);
                    } else if (action == "clear") {
                        index_ai::cmd_mem_shared_clear(get_memory_dir());
                        output_queue.push_msg("Shared scratchpad cleared");
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
                        output_queue.push_msg("\033[38;5;167m" + result + "\033[0m");
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
                            output_queue.push_msg(index_ai::render_markdown(resp.content));
                            tracker.record(current_agent, orch.get_agent_model(current_agent), resp);
                            tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
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
                output_queue.push_msg("\033[2m[plan] executing: " + path + "]\033[0m");
                auto result = orch.execute_plan(path,
                    [&](const std::string& msg) {
                        output_queue.push_msg("\033[2m" + msg + "\033[0m");
                    });
                if (!result.ok) {
                    output_queue.push_msg("\033[38;5;167m[plan] failed: " + result.error + "\033[0m");
                } else {
                    output_queue.push_msg("\033[2m[plan] complete — " +
                                      std::to_string(result.phases.size()) + " phase(s) executed]\033[0m");
                    // Print final phase output (the deliverable)
                    if (!result.phases.empty()) {
                        auto& [num, name, out] = result.phases.back();
                        output_queue.push_msg(index_ai::render_markdown(out));
                    }
                }
                return;
            }
            if (cmd == "help") {
                output_queue.push_msg(
                    "Commands:\n"
                    "  /send <agent> <msg>              — send to specific agent\n"
                    "  /ask <query>                     — ask index master\n"
                    "  /use <agent>                     — switch current agent\n"
                    "  /agents                          — list agents\n"
                    "  /status                          — system status\n"
                    "  /tokens                          — token usage\n"
                    "  /create <id>                     — create agent (default config)\n"
                    "  /remove <id>                     — remove agent\n"
                    "  /reset [id]                      — clear agent history\n"
                    "  /compact [id]                    — summarize + clear history (session memory)\n"
                    "  /model <agent> <model-id>        — change agent model at runtime\n"
                    "\n"
                    "  /loop <agent> <prompt>           — run agent in background loop\n"
                    "  /loops                           — list running/suspended loops\n"
                    "  /log <loop-id> [last-N]          — show buffered loop output\n"
                    "  /watch <loop-id>                 — tail loop output live (Enter to detach)\n"
                    "  /kill <loop-id>                  — stop a loop\n"
                    "  /suspend <loop-id>               — pause a loop\n"
                    "  /resume <loop-id>                — resume a paused loop\n"
                    "  /inject <loop-id> <msg>          — send message into a running loop\n"
                    "\n"
                    "  /fetch <url>                     — fetch URL and send HTML to current agent\n"
                    "  /mem write <text>                — append note to agent memory\n"
                    "  /mem read                        — load memory into agent context\n"
                    "  /mem show                        — print raw memory file\n"
                    "  /mem clear                       — delete agent memory file\n"
                    "  /mem shared write <text>         — write to pipeline-shared scratchpad\n"
                    "  /mem shared read                 — read shared scratchpad\n"
                    "  /mem shared clear                — clear shared scratchpad\n"
                    "\n"
                    "  /plan execute <path>             — execute a planner-produced plan file\n"
                    "\n"
                    "  /quit                            — exit\n"
                    "\n"
                    "Plain text sends to current agent.");
                return;
            }

            output_queue.push_msg("Unknown command. /help for list.");
            return;
        }

        // Plain text → stream to current agent
        try {
            index_ai::MarkdownRenderer md;
            auto resp = orch.send_streaming(current_agent, line,
                [&md, &output_queue](const std::string& chunk) {
                    auto s = md.feed(chunk);
                    if (!s.empty()) output_queue.push(s);
                });
            auto tail = md.flush();
            if (!tail.empty()) output_queue.push(tail);
            // md.flush() guarantees the stream ended on `\n`; one more gives
            // exactly one blank line before the next message.
            output_queue.end_message();
            if (resp.ok) {
                tracker.record(current_agent, orch.get_agent_model(current_agent), resp);
                tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                maybe_generate_title(line, resp.content);
            } else {
                output_queue.push_msg("\033[38;5;167mERR: " + resp.error + "\033[0m");
            }
        } catch (const std::exception& e) {
            output_queue.push_msg("\033[38;5;167mERR: " + std::string(e.what()) + "\033[0m");
        }
        thinking.stop();
    };  // end handle lambda

    // Execution thread: drains the command queue serially.
    std::thread exec_thread([&]() {
        std::string line;
        while (cmd_queue.pop(line)) {
            cmd_queue.set_busy(true);
            handle(line);
            cmd_queue.set_busy(false);
            // Clear the queue indicator now that this item is done.
            // If more items are pending, begin_input will re-show it on the next loop.
            tui.clear_queue_indicator();
        }
    });

    // ── Main readline loop ──────────────────────────────────────────────────
    std::string multiline_accum;           // accumulated prefix from backslash continuation

    // Scroll history back-buffer: bounded, visual-row-aware.  PgUp/PgDn
    // adjust scroll_offset (in VISUAL rows); offset=0 is live.
    index_ai::ScrollBuffer history;
    history.set_cols(tui.cols());
    int scroll_offset      = 0;   // visual rows above the tail (0 = live view)
    int new_while_scrolled = 0;   // visual rows accumulated while scrolled back

    // Welcome card — always shown at startup, whether or not a session was
    // restored.  Rendered into history so it's part of scrollback, and
    // painted live so it's visible before any prompt input.  Dismissed the
    // moment the user sends their first message (see `welcome_visible` in
    // the REPL loop) so it doesn't linger in scrollback alongside real work.
    tui.draw_welcome(history);
    bool welcome_visible = true;

    // ── State exposed to getc_flush_output (called by the pump thread) ────
    g_getc_state.output_queue       = &output_queue;
    g_getc_state.tui                = &tui;
    g_getc_state.history            = &history;
    g_getc_state.scroll_offset      = &scroll_offset;
    g_getc_state.new_while_scrolled = &new_while_scrolled;
    g_getc_state.quit_requested     = &quit_requested;
    g_getc_state.orch               = &orch;
    g_getc_state.multiline_accum    = &multiline_accum;

    // ── Scroll/cancel handlers for the line editor ─────────────────────────
    // The editor reads stdin itself; when it sees a PgUp/PgDn event it
    // defers the actual scrollback update to us via these callbacks.
    auto apply_scroll = [&](int direction, int step) {
        int max_off = history.total_visual_rows();
        if (direction < 0) {
            scroll_offset = std::min(scroll_offset + step, max_off);
        } else {
            scroll_offset = std::max(0, scroll_offset - step);
            new_while_scrolled = 0;
            if (scroll_offset == 0) tui.clear_status();
        }
        tui.render_scrollback(history, scroll_offset, new_while_scrolled);
    };
    editor.set_scroll_handler(apply_scroll);
    editor.set_cancel_handler([&]() {
        orch.cancel();
        multiline_accum.clear();
        output_queue.push_msg("\033[38;5;167m[interrupted]\033[0m");
    });

    // ── Output pump ────────────────────────────────────────────────────────
    // libedit's blocking readline() on macOS does NOT route through
    // rl_getc_function — it reads el_infile directly via its internal
    // read_char — so we cannot rely on filtered_getc running during idle to
    // flush output_queue to the scroll region.  Instead, a dedicated pump
    // thread ticks every 30 ms and drains the queue.  It shares tui.tty_mutex
    // with the echo path and exec-thread TUI updates so concurrent writes to
    // stdout don't tear each other's ANSI save/restore pairs.
    std::atomic<bool> pump_stop{false};
    std::thread output_pump([&]() {
        while (!pump_stop.load()) {
            // Handle SIGWINCH — our line editor runs on the main thread, so
            // we only need to redraw the TUI chrome and reflow the scroll
            // buffer's wrap cache here.  The editor picks up the new width
            // on the next redraw through tui.cols().
            if (g_winch) {
                g_winch = 0;
                tui.resize();
                history.set_cols(tui.cols());
                // resize() cleared the screen and only redrew chrome — repaint
                // the scroll region from history so the welcome card + any
                // prior output survives terminal resizes (incl. the implicit
                // SIGWINCH some emulators fire on initial attach).
                tui.render_scrollback(history, scroll_offset, new_while_scrolled);
            }
            getc_flush_output();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        getc_flush_output();   // final drain on shutdown
    });

    // Service a pending confirm request from the exec thread.  Returns true
    // if one was handled (caller should loop back without treating read_line's
    // return as EOF).  Called in TWO places: (a) before entering read_line,
    // since editor.interrupt() can fire after the previous iteration returned
    // but before we re-enter — read_line resets interrupt_flag_ on entry, so
    // the interrupt would otherwise be lost; (b) after read_line returns
    // false, for the case where the interrupt arrived DURING read_line.
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

        std::string rendered =
            "\n\033[38;5;214m" + conf_prompt + " [y/N] \033[0m";
        history.push(rendered);
        {
            std::lock_guard<std::recursive_mutex> lk(tui.tty_mutex());
            printf("\0337");
            printf("\033[%d;1H", tui.last_scroll_row());
            fwrite_crlf(rendered);
            printf("\0338");
            fflush(stdout);
        }
        unsigned char ch = 0;
        ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        bool yes = (n == 1) && (ch == 'y' || ch == 'Y');
        // Render the answer as a standalone status line, NOT the raw y/n.
        // Leading \n is critical — the cursor move lands us on the row that
        // already holds the prompt, and writing any glyph there overwrites
        // its first character.  A leading newline scrolls the region up
        // first so the status lands on a fresh row below the prompt.
        // Green for accept, red for deny — semantic, matches the rest of
        // the TUI's color vocabulary (214 orange for the question itself).
        std::string answer = yes
            ? std::string("\n\033[38;5;108m[user accepted input]\033[0m\n")
            : std::string("\n\033[38;5;167m[user denied input]\033[0m\n");
        history.push(answer);
        {
            std::lock_guard<std::recursive_mutex> lk(tui.tty_mutex());
            printf("\0337");
            printf("\033[%d;1H", tui.last_scroll_row());
            fwrite_crlf(answer);
            printf("\0338");
            fflush(stdout);
        }
        pending->set_value(yes);
        return true;
    };

    while (!quit_requested) {
        // Before entering read_line, check if the exec thread queued a
        // confirm while we were away.  Handling it here closes the race
        // where interrupt() fires between iterations and read_line's reset
        // would swallow the flag.
        while (service_confirm()) {}

        tui.begin_input([&cmd_queue]() { return cmd_queue.pending(); });

        // Continuation prompt ("…") when accumulating a multi-line input.
        std::string prompt = multiline_accum.empty()
            ? tui.build_prompt()
            : "\001\033[38;5;241m\002…\001\033[0m\002 ";

        // LineEditor owns stdin while reading.  Page-scroll events are
        // dispatched to our scroll handler and don't return from here;
        // Enter returns with the full line, Ctrl-D on an empty buffer
        // returns false (treated as EOF below).
        std::string line;
        if (!editor.read_line(prompt, line)) {
            if (service_confirm()) continue;
            break;   // real EOF
        }
        if (quit_requested) break;
        if (!line.empty()) editor.add_to_history(line);

        // Return to live view whenever the user submits input.
        scroll_offset      = 0;
        new_while_scrolled = 0;

        // ── Backslash-continuation: "\" at end-of-line accumulates and
        //    re-prompts with "…" until a line arrives without trailing "\".
        if (!line.empty() && line.back() == '\\') {
            multiline_accum += line.substr(0, line.size() - 1) + "\n";
            continue;
        }
        line = multiline_accum + line;
        multiline_accum.clear();

        if (line.empty()) continue;

        // Exit check
        {
            std::string lower = line;
            for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
            while (!lower.empty() && lower.back()  == ' ') lower.pop_back();
            while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
            if (lower == "exit" || lower == "quit" || lower == "q" ||
                lower == "bye"  || lower == ":q"   ||
                lower == "/quit"|| lower == "/exit" || lower == "/q") {
                // Always quit — cancel any in-flight work so the exec thread
                // unblocks and join()s cleanly during shutdown.  ESC handles
                // the "cancel without exiting" case; having exit ALSO require
                // confirmation was redundant and trapped users behind a
                // blocking agent call.
                orch.cancel();
                cmd_queue.drain();
                break;
            }
        }

        // First real message dismisses the welcome card — clear both the
        // scroll buffer (so PgUp doesn't resurrect it) and the on-screen
        // scroll region (so the user's echo starts on a blank canvas rather
        // than appearing below the card).
        if (welcome_visible) {
            welcome_visible = false;
            history.clear();
            scroll_offset      = 0;
            new_while_scrolled = 0;
            tui.clear_scroll_region();
        }

        // Echo the user's prompt into the scroll region so it persists in the
        // session view rather than disappearing when enter is pressed.
        // Styled with a muted arrow prefix to distinguish it from model output.
        // Route the user-input echo through the output_queue so the pump
        // owns every scroll-region write.  The prior version did a lockless
        // pre-drain and then a separate direct stdout write to paint the
        // echo — if the pump thread was mid-write under tty_mutex at the
        // same moment, those two printf sequences could interleave and leave
        // fragments of the bottom separator inside the scroll region.
        // Queue ordering is FIFO, so exec-thread output still appears after
        // the user's echo, and the pump serialises all writes.
        output_queue.push_msg(
            "\033[38;5;244m> \033[38;5;250m" + line + "\033[0m");

        bool was_busy = cmd_queue.is_busy();
        cmd_queue.push(line);
        if (was_busy) {
            tui.set_status("queued (" + std::to_string(cmd_queue.pending()) + " waiting)");
        }
    }

    cmd_queue.stop();
    exec_thread.join();

    // Stop the output pump after the exec thread has drained — that way any
    // final output it pushed still gets rendered before we restore the screen.
    pump_stop = true;
    output_pump.join();

    // Restore stdin to its original termios.
    if (stdin_is_tty) ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_stdin_tm);

    // Persist history (the editor holds the authoritative list now).
    {
        std::ofstream hf(get_config_dir() + "/history");
        for (auto& h : editor.history()) hf << h << '\n';
    }
    orch.save_session(session_path);

    // Restore terminal and print session summary.
    tui.shutdown();
    std::cout << "\n";
    if (tracker.session_cost() > 0.0) {
        std::cout << tracker.format_summary();
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            cmd_interactive();
            return 0;
        }

        std::string arg1 = argv[1];

        if (arg1 == "--init" || arg1 == "init") {
            index_ai::cmd_init();
            return 0;
        }
        if (arg1 == "--gen-token" || arg1 == "gen-token") {
            index_ai::cmd_gen_token();
            return 0;
        }
        if (arg1 == "--serve" || arg1 == "serve") {
            int port = 9077;
            if (argc >= 4 && std::string(argv[2]) == "--port") {
                port = std::atoi(argv[3]);
            }
            index_ai::cmd_serve(port);
            return 0;
        }
        if (arg1 == "--send" || arg1 == "send") {
            if (argc < 4) {
                std::cerr << "Usage: index --send <agent_id> <message>\n";
                return 1;
            }
            std::string agent = argv[2];
            std::string msg;
            for (int i = 3; i < argc; ++i) {
                if (i > 3) msg += " ";
                msg += argv[i];
            }
            index_ai::cmd_oneshot(agent, msg);
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h" || arg1 == "help") {
            std::cout << BANNER;
            std::cout <<
                "Usage:\n"
                "  index                          Interactive REPL\n"
                "  index --serve [--port N]        Start TCP server (default 9077)\n"
                "  index --send <agent> <msg>      One-shot message\n"
                "  index --init                    Initialize config + tokens\n"
                "  index --gen-token               Generate new auth token\n"
                "  index --help                    This help\n\n"
                "Environment:\n"
                "  ANTHROPIC_API_KEY                  Claude API key\n\n"
                "Config: ~/.index/\n"
                "  api_key                            API key file\n"
                "  auth_tokens                        Hashed access tokens\n"
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
