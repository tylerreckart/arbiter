#pragma once
// Shared helpers for y/N confirms and Esc/Ctrl-C abandon during cancel-wait
// (#46). Header-only so unit tests can cover kitty CSI-u decoding without
// linking the full TUI binary.

#include "tui/opentui/kitty_key_decode.h"

#include <string>

namespace arbiter {

// True for bare Esc / Ctrl-C, including kitty CSI-u encodings
// (Esc → CSI 27 u, Ctrl-C → CSI 99;5 u).
inline bool is_abandon_key(int key, char csi_final, const std::string& csi_params) {
    if (key == 0x03) return true;  // legacy Ctrl-C
    if (key == 0x1B && csi_final == 0 && csi_params.empty()) return true;  // bare Esc
    if (key == 0x1B && csi_final == 'u') {
        if (auto legacy = opentui::decode_kitty_csi_u(csi_params)) {
            return *legacy == 0x1B || *legacy == 0x03;
        }
    }
    return false;
}

}  // namespace arbiter
