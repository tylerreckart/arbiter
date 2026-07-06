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

} // namespace arbiter
