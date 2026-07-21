// Pure logic split out of transcript_replay.cpp so it can be unit tested
// without linking the OpenTUI-backed PaneScrollView machinery the rest of
// replay needs (createTextBuffer et al. from the native opentui library) —
// see tests/test_transcript_replay_predicates.cpp.
#include "repl/transcript_replay.h"

namespace arbiter {

std::size_t replay_tail_begin(std::size_t total) {
    return (total > kReplayTailMessages) ? total - kReplayTailMessages : 0;
}

// [TOOL RESULTS]/[PANE RESULT] re-entry frames are mechanically fed back to
// the agent as "user" messages by the dispatch loop and the delegated-pane
// flow (main.cpp's start_pane_thread) — they're runtime plumbing, not
// something the user typed, so replayed scrollback should skip them just
// like it skips writs.
bool is_replay_noise(const Message& m) {
    if (m.role != "user") return false;
    return m.content.find("[END TOOL RESULTS]") != std::string::npos
        || m.content.find("[END PANE RESULT]") != std::string::npos;
}

std::string_view replay_user_echo_text(const Message& m) {
    std::string_view content = m.content;
    // Orchestrator::send_streaming / send_internal prepend
    //   [optional OPEN TODOS / lesson blocks]
    //   AGENTS …\n\nQUERY: <user text>
    // for the master ("index") agent. Live UI echoes only <user text>.
    static constexpr std::string_view kQueryMark = "\n\nQUERY: ";
    const auto q = content.rfind(kQueryMark);
    if (q == std::string_view::npos) return content;
    const std::string_view head = content.substr(0, q);
    // Require an AGENTS roster line in the preamble so a user who literally
    // typed "\n\nQUERY: …" is not stripped.
    if (head.find("AGENTS") == std::string_view::npos) return content;
    std::string_view body = content.substr(q + kQueryMark.size());
    // Agent::concatenate_text joins the "QUERY: " prefix part to the user
    // text part with an inserted '\n' (prefix does not end with newline).
    // That leading newline would become an empty echo row inside the band
    // on replay only — live echoes never see it. Strip it.
    while (!body.empty() && (body.front() == '\n' || body.front() == '\r')) {
        if (body.front() == '\r' && body.size() > 1 && body[1] == '\n') {
            body.remove_prefix(2);
        } else {
            body.remove_prefix(1);
        }
    }
    return body;
}

} // namespace arbiter
