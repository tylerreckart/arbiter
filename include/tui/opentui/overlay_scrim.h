#pragma once
// Modal dim is implemented by tui_begin_modal_dim() / tui_end_modal_dim()
// (see tui_design.h).  OpenTUI cannot alpha-blend over already-painted
// cells, so we never fill the frame with an opaque veil.

#include "tui/tui_design.h"

namespace arbiter::opentui {

// Keep chrome + scrollback dimmed while a modal is up.  Returns true when
// the published design changed (caller should apply / retheme).
inline bool sync_modal_dim(bool want_dim) {
    if (want_dim) return tui_begin_modal_dim();
    return tui_end_modal_dim();
}

} // namespace arbiter::opentui
