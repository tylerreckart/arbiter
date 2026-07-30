#pragma once
// Terminal clipboard helpers (OSC 52). Used to copy scrollback selections
// without leaving the alt screen / raw mode.

#include <string_view>

namespace arbiter {

// Write `text` to the terminal clipboard via OSC 52 (`OSC 52 ; c ; <b64> BEL`).
// Best-effort: returns false when the write fails. Host support varies
// (kitty/ghostty/iTerm2 usually work; tmux needs `set -g set-clipboard on`).
[[nodiscard]] bool clipboard_write_osc52(std::string_view text);

}  // namespace arbiter
