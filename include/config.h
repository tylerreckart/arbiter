#pragma once
// arbiter/include/config.h — Per-session runtime configuration
//
// Lives in cmd_interactive() and is referenced (by pointer/reference) by the
// stream filter and the /verbose command handler.  Not persisted — toggles
// reset on restart by design.

namespace arbiter {

struct Config {
    // When true, the agent's raw /cmd lines stream through to scrollback as
    // the model emits them.  When false (default), those lines are swallowed
    // by StreamFilter and ToolCallIndicator surfaces a single "N tool calls…"
    // spinner in the status bar for the duration of the turn.
    bool verbose = false;
};

} // namespace arbiter
