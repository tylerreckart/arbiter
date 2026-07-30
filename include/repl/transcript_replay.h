#pragma once

#include "api_client.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace arbiter {

struct Pane;

// Trailing messages replayed on a conversation switch, and the chunk size
// loaded per PgUp step past the gap marker (see pane_history_load_gap_chunk
// in pane_history.h).
inline constexpr std::size_t kReplayTailMessages = 50;
inline constexpr std::size_t kReplayChunkMessages = 50;

// Start index of the tail window replayed on a switch, given a history of
// `total` messages — 0 if the whole history fits within kReplayTailMessages.
[[nodiscard]] std::size_t replay_tail_begin(std::size_t total);

// Layout restore key for transcript replay. Live ^W splits inherit the parent
// conversation but keep an empty scrollback; relaunch must replay each
// (conversation_id, agent) binding at most once (pre-order first leaf wins)
// so empty sibling windows are not polluted with the parent transcript.
[[nodiscard]] inline std::string pane_transcript_replay_key(
    std::string_view conversation_id, std::string_view agent) {
    const std::string_view a = agent.empty() ? "index" : agent;
    std::string key;
    key.reserve(conversation_id.size() + a.size() + 1);
    key.append(conversation_id);
    key.push_back('\n');
    key.append(a);
    return key;
}

// Inserts the binding into `claimed`. Returns true on first claim (caller
// should replay), false if a sibling pane already owns this transcript.
[[nodiscard]] inline bool claim_pane_transcript_replay(
    std::unordered_set<std::string>& claimed,
    std::string_view conversation_id,
    std::string_view agent) {
    return claimed.insert(pane_transcript_replay_key(conversation_id, agent))
        .second;
}

// True for "user"-role messages that are mechanical re-entry plumbing
// ([TOOL RESULTS]/[PANE RESULT] frames the dispatch loop feeds back to the
// agent), not something the user actually typed — replay skips these.
[[nodiscard]] bool is_replay_noise(const Message& m);

// Text to echo for a user history message. Master turns store
// `global_status() + "\\n\\nQUERY: " + user_text` (and optionally an
// [OPEN TODOS] / lesson preamble) in history; live scrollback only echoes
// the raw user line. Replay strips that orchestrator prefix so switch
// matches the initial session view.
[[nodiscard]] std::string_view replay_user_echo_text(const Message& m);

// Renders messages [begin, end) of a conversation's message history into
// pane's scrollback through the same StreamRenderer/BlockParser pipeline a
// live turn uses (kReplay policy), so replayed output is visually identical
// to live output and picks up the current theme automatically. User
// messages get the same "> "-prefixed echo styling live input uses;
// mechanical re-entry frames ([TOOL RESULTS]/[PANE RESULT] plumbing, not
// real user turns) are skipped. Drains into pane.scroll before returning.
// If `begin` > 0, leaves a gap marker at the front of scrollback for
// replay_load_previous_chunk() to expand later.
void replay_transcript(Pane& pane,
                       const std::vector<Message>& history,
                       std::size_t begin,
                       std::size_t end);

// Loads the previous kReplayChunkMessages messages behind pane's gap
// marker, prepending them to scrollback and adjusting pane.scroll_offset by
// the number of rows added so the viewport doesn't jump. `history` must be
// the same message vector (or an equivalent one) replay_transcript() was
// called with. Returns false (no-op) if there's no gap to load.
bool replay_load_previous_chunk(Pane& pane, const std::vector<Message>& history);

} // namespace arbiter
