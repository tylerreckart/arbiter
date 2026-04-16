// index_ai/src/commands.cpp — Agent-invocable command execution
#include "commands.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace index_ai {

// ---------------------------------------------------------------------------
// parse_agent_commands
// ---------------------------------------------------------------------------

std::vector<AgentCommand> parse_agent_commands(const std::string& response) {
    std::vector<AgentCommand> result;
    std::istringstream ss(response);
    std::string line;
    bool in_code_block = false;

    while (std::getline(ss, line)) {
        // Track code fences (``` or ~~~)
        if (line.size() >= 3 &&
            (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~")) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) continue;

        // Trim trailing whitespace / CR
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.pop_back();

        if (line.size() > 7 && line.substr(0, 7) == "/fetch ") {
            AgentCommand cmd;
            cmd.name = "fetch";
            cmd.args = line.substr(7);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 5 && line.substr(0, 5) == "/mem ") {
            AgentCommand cmd;
            cmd.name = "mem";
            cmd.args = line.substr(5);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 7 && line.substr(0, 7) == "/agent ") {
            AgentCommand cmd;
            cmd.name = "agent";
            cmd.args = line.substr(7);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 8 && line.substr(0, 8) == "/advise ") {
            AgentCommand cmd;
            cmd.name = "advise";
            cmd.args = line.substr(8);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 6 && line.substr(0, 6) == "/exec ") {
            AgentCommand cmd;
            cmd.name = "exec";
            cmd.args = line.substr(6);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 7 && line.substr(0, 7) == "/write ") {
            // Multiline write block: /write <path>\n<content>\n/endwrite
            AgentCommand cmd;
            cmd.name = "write";
            cmd.args = line.substr(7);
            // Trim trailing whitespace from path
            while (!cmd.args.empty() && (cmd.args.back() == ' ' || cmd.args.back() == '\r'))
                cmd.args.pop_back();
            if (cmd.args.empty()) continue;

            // Accumulate lines until /endwrite
            std::ostringstream body;
            bool closed = false;
            while (std::getline(ss, line)) {
                // Trim CR
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "/endwrite") { closed = true; break; }
                body << line << "\n";
            }
            // Remove trailing newline added by the loop
            cmd.content = body.str();
            if (!cmd.content.empty() && cmd.content.back() == '\n')
                cmd.content.pop_back();
            // Mid-stream cutoff: no /endwrite sentinel ⇒ body is incomplete.
            // Orchestrator uses this to request a continuation before writing.
            cmd.truncated = !closed;
            result.push_back(std::move(cmd));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// html_to_text — strip tags and boilerplate, return readable text
// ---------------------------------------------------------------------------

static std::string html_to_text(const std::string& html) {
    std::string out;
    out.reserve(html.size() / 4);

    size_t i = 0;
    const size_t n = html.size();

    // Skip script/style blocks wholesale
    auto skip_block = [&](const char* close_tag) {
        size_t pos = html.find(close_tag, i);
        i = (pos == std::string::npos) ? n : pos + std::strlen(close_tag);
    };

    bool last_was_space = true;

    while (i < n) {
        if (html[i] == '<') {
            // Peek at the tag name
            size_t j = i + 1;
            while (j < n && html[j] == ' ') ++j;
            // Check for block-level tags we want to map to newlines
            auto tag_is = [&](const char* t) {
                size_t tl = std::strlen(t);
                return n - j >= tl &&
                       ::strncasecmp(html.c_str() + j, t, tl) == 0 &&
                       (j + tl >= n || html[j + tl] == '>' || html[j + tl] == ' ');
            };
            if (tag_is("script") || tag_is("style") || tag_is("noscript")) {
                // Find the matching close tag
                size_t close = html.find('>', i);
                if (close != std::string::npos) i = close + 1;
                // Now skip until </script>, </style>, </noscript>
                if (tag_is("script"))   { skip_block("</script>");   continue; }
                if (tag_is("style"))    { skip_block("</style>");    continue; }
                if (tag_is("noscript")) { skip_block("</noscript>"); continue; }
            }
            bool block = tag_is("p")  || tag_is("/p")  ||
                         tag_is("br") || tag_is("li")  ||
                         tag_is("h1") || tag_is("h2")  || tag_is("h3") ||
                         tag_is("h4") || tag_is("h5")  || tag_is("h6") ||
                         tag_is("div") || tag_is("/div") ||
                         tag_is("tr") || tag_is("td")  || tag_is("th");
            // Skip to end of tag
            while (i < n && html[i] != '>') ++i;
            if (i < n) ++i;
            if (block && !out.empty() && out.back() != '\n') {
                out += '\n';
                last_was_space = true;
            }
        } else if (html[i] == '&') {
            // Basic HTML entity decoding
            if (n - i >= 4 && html.substr(i, 4) == "&lt;")       { out += '<'; i += 4; }
            else if (n - i >= 4 && html.substr(i, 4) == "&gt;")  { out += '>'; i += 4; }
            else if (n - i >= 5 && html.substr(i, 5) == "&amp;") { out += '&'; i += 5; }
            else if (n - i >= 6 && html.substr(i, 6) == "&nbsp;"){ out += ' '; i += 6; last_was_space = true; }
            else { out += html[i++]; last_was_space = false; }
        } else {
            char c = html[i++];
            if (c == '\r') continue;
            bool is_ws = (c == ' ' || c == '\t' || c == '\n');
            if (is_ws) {
                if (!last_was_space && !out.empty()) {
                    out += (c == '\n') ? '\n' : ' ';
                    last_was_space = true;
                }
            } else {
                out += c;
                last_was_space = false;
            }
        }
    }

    // Collapse runs of blank lines to a single blank line
    std::string compressed;
    compressed.reserve(out.size());
    int consecutive_newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            ++consecutive_newlines;
            if (consecutive_newlines <= 2) compressed += c;
        } else {
            consecutive_newlines = 0;
            compressed += c;
        }
    }

    return compressed;
}

// ---------------------------------------------------------------------------
// cmd_fetch
// ---------------------------------------------------------------------------

std::string cmd_fetch(const std::string& url) {
    // Must start with http:// or https://
    if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://")
        return "ERR: URL must start with http:// or https://";

    // Reject shell metacharacters to prevent injection
    for (char c : url) {
        if (c == '\'' || c == '"' || c == '`' || c == '$' ||
            c == ';'  || c == '&' || c == '|' || c == '>' ||
            c == '<'  || c == '\n'|| c == '\r') {
            return "ERR: URL contains invalid characters";
        }
    }

    std::string cmd = "curl -sL --max-time 15 --max-filesize 524288 '" + url + "' 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "ERR: failed to run curl";

    std::string raw;
    raw.reserve(65536);
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        raw += buf;
        if (raw.size() > 512 * 1024) break;
    }
    pclose(pipe);

    // Strip HTML tags and boilerplate to save tokens
    std::string result = html_to_text(raw);
    return result;
}

// ---------------------------------------------------------------------------
// cmd_exec
// ---------------------------------------------------------------------------

std::string cmd_exec(const std::string& command) {
    static constexpr size_t kMaxOutput = 32768;

    if (command.empty()) return "ERR: empty command";

    // Capture stdout and stderr together
    std::string shell_cmd = command + " 2>&1";
    FILE* pipe = popen(shell_cmd.c_str(), "r");
    if (!pipe) return "ERR: popen failed";

    std::string output;
    output.reserve(4096);
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
        if (output.size() > kMaxOutput) {
            output.resize(kMaxOutput);
            output += "\n... [truncated at 32 KB]";
            break;
        }
    }
    int status = pclose(pipe);

    // Trim trailing newlines
    while (!output.empty() && output.back() == '\n')
        output.pop_back();

    if (output.empty()) output = "(no output)";

    if (status != 0) {
        output += "\n[exit " + std::to_string(status) + "]";
    }

    return output;
}

// ---------------------------------------------------------------------------
// cmd_write
// ---------------------------------------------------------------------------

std::string cmd_write(const std::string& path, const std::string& content) {
    if (path.empty()) return "ERR: empty path";

    // Basic path safety: reject obvious traversal tricks
    if (path.find("..") != std::string::npos)
        return "ERR: path traversal not permitted";

    // Create parent directories
    fs::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec) return "ERR: cannot create directories: " + ec.message();
    }

    // Back up existing file before overwriting.
    bool overwrite = false;
    std::string bak_note;
    if (fs::exists(p)) {
        overwrite = true;
        std::string bak = path + ".bak";
        std::error_code ec;
        fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ec);
        if (!ec) bak_note = " (previous saved to " + bak + ")";
    }

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) return "ERR: cannot open for writing: " + path;

    f << content;
    if (!content.empty() && content.back() != '\n') f << '\n';

    if (f.fail()) return "ERR: write failed: " + path;
    f.close();

    // Verify: read the file back and compare bytes.  Catches silent
    // disk-level truncation (partial fs writes, ENOSPC, etc.) and gives
    // the agent a concrete signal when the file on disk doesn't match
    // what it intended to write.
    size_t expected = content.size();
    if (!content.empty() && content.back() != '\n') ++expected;  // we added one

    std::ifstream v(path, std::ios::binary | std::ios::ate);
    if (!v.is_open())
        return "ERR: wrote " + std::to_string(content.size())
             + " bytes to " + path + " but cannot re-open to verify";

    size_t on_disk = static_cast<size_t>(v.tellg());
    v.close();

    if (on_disk != expected)
        return "ERR: write verification failed for " + path
             + " (expected " + std::to_string(expected)
             + " bytes, found " + std::to_string(on_disk) + ")";

    std::string action = overwrite ? "overwrote" : "wrote";
    return "OK: " + action + " " + std::to_string(content.size())
           + " bytes to " + path + bak_note + " (verified)";
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

std::string cmd_mem_read(const std::string& agent_id, const std::string& memory_dir) {
    std::string path = memory_dir + "/" + agent_id + ".md";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string cmd_mem_write(const std::string& agent_id, const std::string& text,
                          const std::string& memory_dir) {
    std::error_code ec;
    fs::create_directories(memory_dir, ec);
    if (ec) return "ERR: cannot create memory dir " + memory_dir + ": " + ec.message();

    std::string path = memory_dir + "/" + agent_id + ".md";
    std::ofstream f(path, std::ios::app);
    if (!f.is_open()) return "ERR: cannot open " + path + " for writing";

    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    f << "\n<!-- " << ts << " -->\n" << text << "\n";
    if (f.fail()) return "ERR: write failed for " + path;
    return "OK: memory written to " + path;
}

void cmd_mem_clear(const std::string& agent_id, const std::string& memory_dir) {
    std::string path = memory_dir + "/" + agent_id + ".md";
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Shared scratchpad — pipeline-scoped, visible to all agents
// ---------------------------------------------------------------------------

static std::string shared_mem_path(const std::string& memory_dir) {
    return memory_dir + "/shared.md";
}

std::string cmd_mem_shared_read(const std::string& memory_dir) {
    std::ifstream f(shared_mem_path(memory_dir));
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void cmd_mem_shared_write(const std::string& text, const std::string& memory_dir) {
    fs::create_directories(memory_dir);
    std::ofstream f(shared_mem_path(memory_dir), std::ios::app);
    if (!f.is_open()) return;
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    f << "\n<!-- " << ts << " -->\n" << text << "\n";
}

void cmd_mem_shared_clear(const std::string& memory_dir) {
    fs::remove(shared_mem_path(memory_dir));
}

// ---------------------------------------------------------------------------
// execute_agent_commands
// ---------------------------------------------------------------------------

// Conservative pattern check — matches commonly-destructive shell forms.
// Expanding this is cheap; the callsite bails to a y/N prompt on a match.
bool is_destructive_exec(const std::string& cmd) {
    // Normalise: lowercase + collapse whitespace, preserve literal chars like '>'.
    std::string s;
    s.reserve(cmd.size());
    bool prev_space = true;
    for (char c : cmd) {
        char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        if (lc == ' ' || lc == '\t') {
            if (!prev_space) { s += ' '; prev_space = true; }
        } else {
            s += lc;
            prev_space = false;
        }
    }
    if (!s.empty() && s.back() == ' ') s.pop_back();
    // Pad with spaces so word-boundary checks (" rm ") hit tokens at the ends.
    std::string padded = " " + s + " ";

    auto has = [&](const char* needle) {
        return padded.find(needle) != std::string::npos;
    };

    // Destructive filesystem + process tools.
    if (has(" rm ") || has(" rmdir ") || has(" unlink ") ||
        has(" shred ") || has(" truncate ") ||
        has(" dd ")  || has(" mkfs")      || has(" wipefs") ||
        has(" fdisk ") || has(" parted ") ||
        has(" chmod -r") || has(" chown -r")) return true;

    // Privilege escalation — anything can happen past sudo.
    if (has(" sudo ") || has(" doas ")) return true;

    // Shell redirection overwrites/appends files.  We gate both since an
    // agent-issued `>` is the moral equivalent of /write.
    if (padded.find(" > ") != std::string::npos ||
        padded.find(">>") != std::string::npos) return true;

    // find -delete / find -exec rm …
    if (has(" find ") && (has(" -delete") ||
                          padded.find(" -exec rm") != std::string::npos ||
                          padded.find(" -execdir rm") != std::string::npos))
        return true;

    // Git operations that rewrite or discard history.
    if (has(" git ")) {
        if (has(" reset --hard") || has(" clean -f") ||
            has(" push --force") || has(" push -f ") ||
            has(" branch -d") || has(" branch -D") ||
            has(" checkout --") || has(" restore .")) return true;
    }

    return false;
}

std::string execute_agent_commands(const std::vector<AgentCommand>& cmds,
                                   const std::string& agent_id,
                                   const std::string& memory_dir,
                                   AgentInvoker agent_invoker,
                                   ConfirmFn    confirm,
                                   std::map<std::string, std::string>* dedup_cache,
                                   AdvisorInvoker advisor_invoker) {
    std::ostringstream out;
    out << "\n";

    // Caps: 16 KB per fetch (stripped text), max 3 fetches per turn,
    // and a total tool-result budget of 32 KB.
    static constexpr size_t kPerFetchLimit  = 16384;
    static constexpr size_t kTotalLimit     = 32768;
    static constexpr int    kMaxFetches     = 3;
    int fetch_count = 0;

    for (auto& cmd : cmds) {
        // Enforce total budget
        if (out.tellp() >= static_cast<std::streampos>(kTotalLimit)) {
            out << "[TOOL RESULTS TRUNCATED: budget exhausted]\n\n";
            break;
        }

        // ── Dedup gate ────────────────────────────────────────────────────
        // If the same (cmd, args[, content]) already ran this turn, drop the
        // repeat silently — do NOT re-dispatch and do NOT emit anything for
        // the user or the model.  Distinct content for /write bypasses the
        // dedup so writing two different files to the same path still works.
        std::string dedup_key = cmd.name + "|" + cmd.args;
        if (cmd.name == "write") {
            dedup_key += "|" + std::to_string(std::hash<std::string>{}(cmd.content));
        }
        if (dedup_cache && dedup_cache->find(dedup_key) != dedup_cache->end()) {
            continue;
        }

        // Capture each command's full block (header + body + footer) in a
        // local stream so we can feed it to the dedup cache after it runs.
        std::ostringstream block;

        // Set to false when the result shouldn't be cached for dedup — e.g.
        // fetch-budget-skip, or user-declined confirms: a later re-issue is
        // a legitimate retry, not a duplicate.
        bool cache_result = true;

        if (cmd.name == "fetch") {
            if (fetch_count >= kMaxFetches) {
                block << "[/fetch " << cmd.args << "]\n"
                      << "SKIPPED: max " << kMaxFetches << " fetches per turn\n"
                      << "[END FETCH]\n\n";
                cache_result = false;
            } else {
                ++fetch_count;
                block << "[/fetch " << cmd.args << "]\n";
                std::string fetched = cmd_fetch(cmd.args);
                if (fetched.size() > kPerFetchLimit) {
                    fetched.resize(kPerFetchLimit);
                    fetched += "\n... [truncated]";
                }
                block << fetched << "\n";
                block << "[END FETCH]\n\n";
            }

        } else if (cmd.name == "mem") {
            std::istringstream iss(cmd.args);
            std::string subcmd;
            iss >> subcmd;

            if (subcmd == "shared") {
                // /mem shared write <text> | /mem shared read | /mem shared clear
                std::string action;
                iss >> action;
                if (action == "write") {
                    std::string text;
                    std::getline(iss, text);
                    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                    cmd_mem_shared_write(text, memory_dir);
                    block << "[/mem shared write] OK: written to shared scratchpad\n\n";
                } else if (action == "read" || action == "show") {
                    std::string mem = cmd_mem_shared_read(memory_dir);
                    block << "[/mem shared read]\n"
                          << (mem.empty() ? "(shared scratchpad empty)" : mem)
                          << "\n[END SHARED MEMORY]\n\n";
                } else if (action == "clear") {
                    cmd_mem_shared_clear(memory_dir);
                    block << "[/mem shared clear] OK: shared scratchpad cleared\n\n";
                } else {
                    block << "[/mem shared] ERR: unknown action '" << action
                          << "' — use write, read, or clear\n\n";
                    cache_result = false;
                }

            } else if (subcmd == "write") {
                std::string text;
                std::getline(iss, text);
                if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                std::string result = cmd_mem_write(agent_id, text, memory_dir);
                block << "[/mem write] " << result << "\n\n";

            } else if (subcmd == "read") {
                std::string mem = cmd_mem_read(agent_id, memory_dir);
                block << "[/mem read]\n"
                      << (mem.empty() ? "(no memory)" : mem)
                      << "\n[END MEMORY]\n\n";

            } else if (subcmd == "show") {
                std::string mem = cmd_mem_read(agent_id, memory_dir);
                block << "[/mem show]\n"
                      << (mem.empty() ? "(no memory)" : mem)
                      << "\n[END MEMORY]\n\n";

            } else if (subcmd == "clear") {
                cmd_mem_clear(agent_id, memory_dir);
                block << "[/mem clear] OK: memory cleared\n\n";
            }

        } else if (cmd.name == "exec") {
            block << "[/exec " << cmd.args << "]\n";
            if (confirm && is_destructive_exec(cmd.args) &&
                !confirm("exec '" + cmd.args + "'?")) {
                block << "ERR: user declined\n";
                cache_result = false;  // user may want to approve a retry
            } else {
                block << cmd_exec(cmd.args) << "\n";
            }
            block << "[END EXEC]\n\n";

        } else if (cmd.name == "advise") {
            // /advise <question>
            // Fires a one-shot, zero-context call against the caller's
            // configured advisor_model.  Replaces Anthropic's beta
            // `advisor_20260301` tool with a provider-portable path, so
            // cheap executors (ollama/*) can pair with smart advisors
            // (claude-opus-*) without relying on Anthropic-only plumbing.
            block << "[/advise]\n";
            if (!advisor_invoker) {
                block << "ERR: advisor not configured — set advisor_model in "
                         "the agent config to enable /advise.\n";
                cache_result = false;
            } else {
                std::string answer = advisor_invoker(cmd.args);
                block << answer;
                if (!answer.empty() && answer.back() != '\n') block << "\n";
                // Same upstream-failed framing as /agent — don't let the
                // executor burn retries against a failing advisor.
                if (answer.size() >= 4 && answer.compare(0, 4, "ERR:") == 0) {
                    block << "UPSTREAM FAILED — the advisor returned an error. "
                             "Same question will fail again.  Do NOT re-issue "
                             "the identical /advise.  Proceed with your own "
                             "judgment and note the open question in your "
                             "output.\n";
                }
            }
            block << "[END ADVISE]\n\n";

        } else if (cmd.name == "agent") {
            // /agent <sub_agent_id> <message>
            std::istringstream iss(cmd.args);
            std::string sub_id;
            iss >> sub_id;
            std::string sub_msg;
            std::getline(iss, sub_msg);
            if (!sub_msg.empty() && sub_msg[0] == ' ') sub_msg.erase(0, 1);

            block << "[/agent " << sub_id << "]\n";
            if (!agent_invoker) {
                block << "ERR: agent invocation unavailable in this context\n";
            } else {
                std::string sub_resp = agent_invoker(sub_id, sub_msg);
                block << sub_resp << "\n";
                // Clearer failure signal: when the sub-agent returns an upstream
                // error, tell the calling agent NOT to retry the identical brief.
                // The internal ApiClient already burned 4 backoff retries before
                // the sub-agent returned ERR, so a same-turn reinvocation is
                // almost certainly going to fail the same way.  Prescribe the
                // adaptation paths so the master doesn't just repeat itself.
                if (sub_resp.size() >= 4 && sub_resp.compare(0, 4, "ERR:") == 0) {
                    block << "UPSTREAM FAILED — the sub-agent returned an error "
                             "after the API client's internal retry/backoff chain. "
                             "The same brief will fail again.  Do NOT re-invoke "
                             "/agent " << sub_id << " with the same arguments.  "
                             "Options: adapt the brief (narrower scope, different "
                             "angle), try a different agent whose capabilities "
                             "overlap, or proceed with partial results and flag "
                             "the gap in your synthesis.\n";
                }
            }
            block << "[END AGENT]\n\n";

        } else if (cmd.name == "write") {
            block << "[/write " << cmd.args << "]\n";
            // Orchestrator attempted to recover unclosed /write blocks before
            // we got here.  If `truncated` is STILL set, recovery exhausted
            // its retries — refuse to persist the partial and tell the model
            // explicitly so the next turn can retry with a fresh /write.
            if (cmd.truncated) {
                block << "ERR: /write block for " << cmd.args
                      << " was truncated mid-generation (missing /endwrite) "
                         "and could not be recovered.  File NOT written.  "
                         "Re-emit the full /write block from scratch.\n";
                block << "[END WRITE]\n\n";
                cache_result = false;   // allow a clean retry
                out << block.str();
                continue;
            }
            if (confirm) {
                size_t lines = std::count(cmd.content.begin(), cmd.content.end(), '\n');
                if (!cmd.content.empty() && cmd.content.back() != '\n') ++lines;
                std::string p = "write " + cmd.args + " (" +
                                std::to_string(lines) + " lines, " +
                                std::to_string(cmd.content.size()) + " bytes)?";
                if (!confirm(p)) {
                    block << "ERR: user declined\n";
                    block << "[END WRITE]\n\n";
                    cache_result = false;  // user may want to approve a retry
                    out << block.str();
                    continue;
                }
            }
            block << cmd_write(cmd.args, cmd.content) << "\n";
            block << "[END WRITE]\n\n";
        }

        // Flush per-command block into the aggregate and record it in the
        // dedup cache so a same-turn repeat short-circuits to the quote path.
        std::string block_str = block.str();
        out << block_str;
        if (dedup_cache && cache_result && !block_str.empty()) {
            (*dedup_cache)[dedup_key] = block_str;
        }
    }

    out << "[END TOOL RESULTS]";
    return out.str();
}

} // namespace index_ai
