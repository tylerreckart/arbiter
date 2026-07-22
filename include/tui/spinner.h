#pragma once
// Shared Braille loader + rotating wait phrases for TUI status chrome.

#include <string>
#include <string_view>

namespace arbiter {

// Current Braille spinner glyph (advances ~every 80ms).
[[nodiscard]] std::string_view spinner_frame();

// Rotating wait phrase ("Working…", "Looking into that…", …). Advances on a
// slower cadence than the Braille frames so the text stays readable.
[[nodiscard]] std::string_view wait_phrase();

// `"{braille} {phrase}"` — default idle-wait status for ThinkingIndicator.
[[nodiscard]] std::string wait_status_label();

// `"{braille} {label}"` — fixed-label status (tool calls, cancel, fetch, …).
[[nodiscard]] std::string spinner_status_label(std::string_view label);

} // namespace arbiter
