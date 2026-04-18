#pragma once
// index/include/config.h — Per-session runtime configuration
//
// Lives in cmd_interactive() and is referenced (by pointer/reference) by the
// stream filter and the /verbose command handler.  Not persisted — toggles
// reset on restart by design.

namespace index_ai {

struct Config {
    // When true, the agent's raw /cmd lines stream through to scrollback as
    // the model emits them.  When false (default), those lines are swallowed
    // by StreamFilter and ToolCallIndicator surfaces a single "N tool calls…"
    // spinner in the status bar for the duration of the turn.
    bool verbose = false;
};

} // namespace index_ai
