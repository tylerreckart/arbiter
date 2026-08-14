// src/advisor_gate.cpp — Signal parser for the advisor gate.  Kept in its
// own translation unit so the parser can be unit-tested without dragging
// the orchestrator's full dependency graph into the test binary.  The
// runtime gate (Orchestrator::make_advisor_gate_invoker, run_dispatch's
// terminating branch) lives in orchestrator.cpp.

#include "commands.h"

#include <cctype>
#include <string>
#include <utility>

namespace arbiter {

namespace {

// Truncate `s` to `max` bytes without splitting a UTF-8 sequence.  When
// truncated, a marker is included inside the budget so a second call is
// a no-op (size already <= max).
std::string cap_bytes(std::string s, size_t max) {
    if (s.size() <= max) return s;
    static constexpr char kMarker[] = "\n... [truncated]";
    constexpr size_t kMarkerLen = sizeof(kMarker) - 1;
    const size_t keep = (max > kMarkerLen) ? (max - kMarkerLen) : max;
    s.resize(keep);
    if (!s.empty()) {
        size_t i = s.size();
        while (i > 0 &&
               (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
            --i;
        }
        if (i == s.size()) {
            // Last byte is not a continuation — ASCII (keep) or a stray
            // multi-byte lead (drop).
            if (static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
        } else {
            const unsigned char lead = static_cast<unsigned char>(s[i]);
            int need = 1;
            if      ((lead & 0xE0) == 0xC0) need = 2;
            else if ((lead & 0xF0) == 0xE0) need = 3;
            else if ((lead & 0xF8) == 0xF0) need = 4;
            else if (lead >= 0x80)          need = 0;  // invalid lead
            if (need == 0 || s.size() - i < static_cast<size_t>(need))
                s.resize(i);
        }
    }
    if (max > kMarkerLen) s += kMarker;
    return s;
}

// Find the first `<tag>...</tag>` block in `s`, return inner text (trimmed).
// Tag matching is literal — no regex, no case-folding on the tag itself.
std::string extract_tag(const std::string& s, const std::string& tag) {
    std::string open  = "<"  + tag + ">";
    std::string close = "</" + tag + ">";
    auto a = s.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    auto b = s.find(close, a);
    if (b == std::string::npos) return {};
    auto inner = s.substr(a, b - a);
    size_t i = 0, j = inner.size();
    while (i < j && std::isspace(static_cast<unsigned char>(inner[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(inner[j - 1]))) --j;
    return inner.substr(i, j - i);
}

}  // namespace

AdvisorGateOutput parse_advisor_signal(const std::string& reply) {
    AdvisorGateOutput out;
    out.raw = reply;

    auto sig = extract_tag(reply, "signal");
    if (sig.empty()) { out.malformed = true; return out; }

    // Case-insensitive on the signal token only — bodies retain casing.
    std::string upper = sig;
    for (auto& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (upper == "CONTINUE") {
        out.kind = AdvisorGateOutput::Kind::Continue;
        return out;
    }
    if (upper == "REDIRECT") {
        out.kind = AdvisorGateOutput::Kind::Redirect;
        out.text = extract_tag(reply, "guidance");
        if (out.text.empty()) out.malformed = true;
        return out;
    }
    if (upper == "HALT") {
        out.kind = AdvisorGateOutput::Kind::Halt;
        out.text = extract_tag(reply, "reason");
        if (out.text.empty()) out.malformed = true;
        return out;
    }

    out.malformed = true;
    return out;
}

void cap_advisor_gate_input(AdvisorGateInput& in) {
    in.original_task    = cap_bytes(std::move(in.original_task),
                                    kAdvisorGateMaxOriginalTask);
    in.terminating_text = cap_bytes(std::move(in.terminating_text),
                                    kAdvisorGateMaxTerminatingText);
    in.tool_summary     = cap_bytes(std::move(in.tool_summary),
                                    kAdvisorGateMaxToolSummary);
}

std::string cap_advisor_prompt_override(std::string prompt) {
    return cap_bytes(std::move(prompt), kAdvisorGateMaxPromptOverride);
}

}  // namespace arbiter
