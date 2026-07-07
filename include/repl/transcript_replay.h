#pragma once

#include "api_client.h"

#include <cstddef>
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

// True for "user"-role messages that are mechanical re-entry plumbing
// ([TOOL RESULTS]/[PANE RESULT] frames the dispatch loop feeds back to the
// agent), not something the user actually typed — replay skips these.
[[nodiscard]] bool is_replay_noise(const Message& m);

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
