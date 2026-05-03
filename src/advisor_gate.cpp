// src/advisor_gate.cpp — Signal parser for the advisor gate.  Kept in its
// own translation unit so the parser can be unit-tested without dragging
// the orchestrator's full dependency graph into the test binary.  The
// runtime gate (Orchestrator::make_advisor_gate_invoker, run_dispatch's
// terminating branch) lives in orchestrator.cpp.

#include "commands.h"

#include <cctype>
#include <string>

namespace index_ai {

namespace {

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

}  // namespace index_ai
