// src/presence.cpp — Always-on peer review: parse, match, format, call.

#include "presence.h"
#include "api_client.h"

#include <algorithm>
#include <cctype>
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
        "You are an always-on presence agent reviewing a snapshot of another "
        "agent's in-flight work.  You do not own the turn.  You cannot halt "
        "or redirect the working agent.  You may only add a short context "
        "note, or stay silent.\n\n"
        "Inputs you receive:\n"
        "  - Your own goal and rules (why you are watching).\n"
        "  - The working agent's id and role.\n"
        "  - The original user task.\n"
        "  - The working agent's most recent assistant text.\n"
        "  - A structured summary of tools it just ran.\n\n"
        "Respond with EXACTLY ONE signal:\n\n"
        "  <signal>SILENT</signal>\n"
        "    Nothing to add.  The working agent should continue unimpeded.\n"
        "    Default here.  Silence is correct when the work is on track, "
        "when you would only repeat what the tools already showed, or when "
        "you are unsure.\n\n"
        "  <signal>CONTEXT</signal>\n"
        "  <note>one or two sentences the working agent should see now</note>\n"
        "    Inject only when YOU hold a fact the working agent was not "
        "given in this snapshot — a prior decision, a house convention, "
        "an artifact or memory entry already on disk, or sibling output "
        "from another agent.  The note must change the next action.  "
        "No preamble.  No markdown.  No questions.\n\n"
        "Do not judge whether the work is good enough to return — that "
        "is the advisor gate (CONTINUE / REDIRECT / HALT).  Do not flag "
        "footguns, retries, or incomplete work.  Do not invent facts.  "
        "Do not restate the tool summary.  Do not congratulate.  "
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
            note.resize(kPresenceMaxNote);
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
    std::string name = watcher_name.empty() ? "presence" : watcher_name;
    std::ostringstream ss;
    ss << "[PRESENCE: " << name << "]\n"
       << note << "\n"
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
        out.text = "no presence model configured";
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
        out.kind = PresenceOutput::Kind::Silent;
        out.malformed = true;
        out.raw = resp.error;
        out.text = "presence API error: " + resp.error;
        return out;
    }
    return parse_presence_signal(resp.content);
}

} // namespace arbiter
