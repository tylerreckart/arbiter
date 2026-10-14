// src/presence.cpp — Always-on peer review: parse, match, format, call.

#include "presence.h"
#include "api_client.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fnmatch.h>
#include <sstream>
#include <string>
#include <string_view>

namespace arbiter {

namespace {

void ascii_tolower_inplace(std::string& s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

std::string trim_copy(std::string_view s) {
    size_t b = 0;
    while (b < s.size() &&
           std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b &&
           std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// Extract the first <tag>…</tag> body (case-insensitive tag name).
// Returns empty when the pair is missing.
std::string first_tag_body(const std::string& text, const char* tag) {
    std::string open = "<";
    open += tag;
    open += ">";
    std::string close = "</";
    close += tag;
    close += ">";
    std::string lower = text;
    ascii_tolower_inplace(lower);
    std::string open_l = open;
    std::string close_l = close;
    ascii_tolower_inplace(open_l);
    ascii_tolower_inplace(close_l);
    auto a = lower.find(open_l);
    if (a == std::string::npos) return {};
    auto body = a + open_l.size();
    auto b = lower.find(close_l, body);
    if (b == std::string::npos) return {};
    return trim_copy(std::string_view(text).substr(body, b - body));
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Keep aligned with starts_with_writ in commands.cpp.  Presence notes are
// untrusted model text; a line that is itself a writ must not survive
// into the working agent's tool envelope.
bool presence_line_is_writ(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    line.remove_prefix(i);
    static const char* kWritPrefixes[] = {
        "/todo", "/endtodo",
        "/agent ", "/parallel", "/endparallel", "/pane ",
        "/write", "/endwrite",
        "/read ", "/list", "/map",
        "/search ", "/fetch ", "/browse ",
        "/exec ",
        "/mem", "/endmem",
        "/mcp ",
        "/a2a", "/schedule",
        "/lesson", "/endlesson",
        "/advise", "/help",
    };
    for (const char* p : kWritPrefixes) {
        const size_t n = std::strlen(p);
        if (line.size() >= n && line.compare(0, n, p) == 0) return true;
        if (n > 0 && p[n - 1] == ' ' &&
            line.size() == n - 1 && line.compare(0, n - 1, p, n - 1) == 0)
            return true;
    }
    return false;
}

// Single header line.  Brackets and controls would let a watcher name
// close the block and plant a slash command under it.
std::string sanitize_presence_name(const std::string& raw) {
    std::string out;
    bool spaced = false;
    for (unsigned char c : raw) {
        // Drop framing, controls, and slashes so a name cannot close the
        // block or carry a `/exec` token into the header line.
        if (c < 0x20 || c == 0x7f || c == '[' || c == ']' || c == '/' || c == '\\') {
            spaced = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            spaced = true;
            continue;
        }
        if (spaced && !out.empty()) out.push_back(' ');
        spaced = false;
        out.push_back(static_cast<char>(c));
        if (out.size() >= 64) break;
    }
    if (out.empty()) return "presence";
    return out;
}

// Drop controls, break `[END PRESENCE]` / `[/exec` framing, and turn a
// leading writ slash into a fullwidth solidus so a copied line is not a
// command the parser accepts.
std::string neutralize_presence_note(const std::string& note) {
    std::string cleaned;
    cleaned.reserve(note.size());
    for (unsigned char c : note) {
        if (c == '\0' || c == '\r' || c == 0x7f) continue;
        if (c < 0x20 && c != '\n' && c != '\t') continue;
        cleaned.push_back(static_cast<char>(c));
    }
    const std::string zwsp = "\xE2\x80\x8B";
    replace_all(cleaned, "[END PRESENCE]", "[END" + zwsp + " PRESENCE]");
    replace_all(cleaned, "[PRESENCE:", "[PRESENCE" + zwsp + ":");
    replace_all(cleaned, "[/", "[" + zwsp + "/");
    replace_all(cleaned, "[END ", "[END" + zwsp + " ");

    std::string out;
    size_t start = 0;
    while (start <= cleaned.size()) {
        const auto nl = cleaned.find('\n', start);
        std::string line = cleaned.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        if (presence_line_is_writ(line)) {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < line.size() && line[i] == '/')
                line.replace(i, 1, "\xEF\xBC\x8F");  // U+FF0F
        }
        out += line;
        if (nl == std::string::npos) break;
        out.push_back('\n');
        start = nl + 1;
        if (start > cleaned.size()) break;
        if (start == cleaned.size()) break;
    }
    return out;
}

void truncate_field(std::string& field, size_t budget) {
    if (field.size() <= budget) return;
    // Leave room for the marker; don't split a trailing UTF-8 sequence.
    const char kMark[] = "\n[truncated]";
    size_t keep = budget > sizeof(kMark) - 1 ? budget - (sizeof(kMark) - 1) : 0;
    while (keep > 0 &&
           (static_cast<unsigned char>(field[keep]) & 0xC0) == 0x80) {
        --keep;
    }
    field.resize(keep);
    field += kMark;
}

}  // namespace

bool presence_watch_matches(const PresenceConfig& cfg,
                            const std::string& agent_id) {
    if (agent_id.empty()) return false;
    if (cfg.watch.empty()) return true;  // empty ≡ all peers
    for (const auto& pattern : cfg.watch) {
        if (pattern.empty()) continue;
        if (fnmatch(pattern.c_str(), agent_id.c_str(), 0) == 0) return true;
    }
    return false;
}

void cap_presence_input(PresenceInput& in) {
    truncate_field(in.original_task, kPresenceMaxOriginalTask);
    truncate_field(in.recent_text, kPresenceMaxRecentText);
    truncate_field(in.tool_summary, kPresenceMaxToolSummary);
    truncate_field(in.watcher_rules, kPresenceMaxRecentText);
    truncate_field(in.watcher_goal, 2048);
}

std::string cap_presence_prompt_override(const std::string& prompt) {
    std::string out = prompt;
    truncate_field(out, kPresenceMaxPromptOverride);
    return out;
}

const char* default_presence_prompt() {
    return
        "You are a pair colleague sitting beside another agent while they "
        "work.  You look over their shoulder.  You do not own the turn, "
        "do not take the keyboard, and cannot halt or redirect them.  "
        "You may only offer a short piece of context that helps them "
        "finish the task at hand, or stay silent.\n\n"
        "You are not specialized in programming unless they are "
        "programming.  Match whatever they are in the middle of — "
        "prose, research, ops, planning, code.\n\n"
        "Inputs you receive:\n"
        "  - Your own goal and rules (how you pair).\n"
        "  - The working agent's id and role.\n"
        "  - The original user task.\n"
        "  - The working agent's most recent assistant text.\n"
        "  - A structured summary of tools it just ran.\n\n"
        "Respond with EXACTLY ONE signal:\n\n"
        "  <signal>SILENT</signal>\n"
        "    Nothing useful to add.  Let them keep working.\n"
        "    Default here.  Silence is correct when the next action is "
        "already clear, when you would only repeat the tool output, or "
        "when you are unsure.\n\n"
        "  <signal>CONTEXT</signal>\n"
        "  <note>one or two sentences that help them finish this task</note>\n"
        "    Speak when you can save a step: a missed connection, a "
        "relevant prior decision, a sibling result, an artifact already "
        "on disk, or a constraint from the original task they are "
        "drifting past.  No preamble.  No markdown.  No questions.\n\n"
        "Do not judge whether the work is good enough to return — that "
        "is the advisor gate (CONTINUE / REDIRECT / HALT).  Do not "
        "verdict footguns, retries, or incomplete work.  Do not invent "
        "facts.  Do not restate the tool summary.  Do not congratulate.  "
        "Prefer SILENT.";
}

PresenceOutput parse_presence_signal(const std::string& reply) {
    PresenceOutput out;
    out.raw = reply;
    std::string token = first_tag_body(reply, "signal");
    if (token.empty()) {
        out.kind = PresenceOutput::Kind::Silent;
        out.malformed = true;
        return out;
    }
    ascii_tolower_inplace(token);
    if (token == "silent") {
        out.kind = PresenceOutput::Kind::Silent;
        return out;
    }
    if (token == "context") {
        std::string note = first_tag_body(reply, "note");
        if (note.empty()) {
            out.kind = PresenceOutput::Kind::Silent;
            out.malformed = true;
            return out;
        }
        if (note.size() > kPresenceMaxNote) {
            truncate_field(note, kPresenceMaxNote);
        }
        out.kind = PresenceOutput::Kind::Context;
        out.text = std::move(note);
        return out;
    }
    out.kind = PresenceOutput::Kind::Silent;
    out.malformed = true;
    return out;
}

std::string format_presence_injection(const std::string& watcher_name,
                                      const std::string& note) {
    const std::string name = sanitize_presence_name(watcher_name);
    const std::string body = neutralize_presence_note(note);
    std::ostringstream ss;
    ss << "[PRESENCE: " << name << "]\n"
       << body << "\n"
       << "[END PRESENCE]\n\n";
    return ss.str();
}

PresenceOutput run_presence_review(
    ApiClient& client,
    const std::string& model,
    const std::string& prompt_override,
    const PresenceInput& in,
    const std::function<void(const ApiResponse&)>& on_response) {

    PresenceOutput out;
    if (model.empty()) {
        out.kind = PresenceOutput::Kind::Silent;
        out.malformed = true;
        return out;
    }

    PresenceInput capped = in;
    cap_presence_input(capped);
    const std::string prompt = cap_presence_prompt_override(prompt_override);

    std::ostringstream q;
    q << "[WATCHER]\n"
      << "id: " << capped.watcher_id << "\n"
      << "name: " << capped.watcher_name << "\n"
      << "goal: " << (capped.watcher_goal.empty() ? "(none)" : capped.watcher_goal)
      << "\n";
    if (!capped.watcher_rules.empty())
        q << "rules:\n" << capped.watcher_rules << "\n";
    q << "[END WATCHER]\n\n"
      << "[WORKING AGENT]\n"
      << capped.working_agent_id;
    if (!capped.working_agent_role.empty())
        q << " [" << capped.working_agent_role << "]";
    q << "\n[END WORKING AGENT]\n\n"
      << "[ORIGINAL TASK]\n" << capped.original_task << "\n[END ORIGINAL TASK]\n\n"
      << "[RECENT ASSISTANT TEXT]\n"
      << (capped.recent_text.empty() ? "(none)\n" : capped.recent_text)
      << "\n[END RECENT ASSISTANT TEXT]\n\n"
      << "[TOOL CALLS THIS TURN]\n"
      << (capped.tool_summary.empty() ? "(none)\n" : capped.tool_summary)
      << "[END TOOL CALLS]\n";

    ApiRequest req;
    req.model               = model;
    req.max_tokens          = 256;
    req.include_temperature = false;
    req.system_prompt       = prompt.empty()
                              ? std::string(default_presence_prompt())
                              : prompt;
    req.messages            = {{"user", q.str()}};

    ApiResponse resp = client.complete(req);
    if (on_response) on_response(resp);
    if (!resp.ok) {
        // Fail-open: a dead watcher must not stall the working agent.
        // Do not copy provider errors into out.text — SSE presence events
        // would leak them to tenants (same policy as sanitised_api_response_error).
        out.kind = PresenceOutput::Kind::Silent;
        out.malformed = true;
        return out;
    }
    return parse_presence_signal(resp.content);
}

} // namespace arbiter
