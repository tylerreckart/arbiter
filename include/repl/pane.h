#pragma once

#include "repl/queues.h"
#include "tui/opentui/pane_input_editor.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/tui.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace arbiter {

struct Pane {
    TUI               tui;
    ThinkingIndicator thinking{&tui};
    ToolCallIndicator tool_indicator{&tui};
    opentui::PaneInputEditor editor{tui};

    OutputQueue       output_queue;
    CommandQueue      cmd_queue;

    std::unique_ptr<opentui::PaneScrollView> scroll;

    std::thread       exec_thread;

    std::string       current_agent = "index";
    std::string       current_model;
    // Conversation this pane is bound to (vim buffer).  Orthogonal to the
    // layout tree (vim window).  Empty until the app assigns one after
    // ConversationStore init.
    std::string       conversation_id;
    std::string       original_task;
    std::string       multiline_accum;
    int               scroll_offset       = 0;
    int               new_while_scrolled  = 0;

    // Unfocused-pane activity (#41): set while a turn runs; latched when a
    // turn completes or new output arrives while this pane isn't focused.
    // Cleared when the pane gains focus.
    std::atomic<bool> turn_running{false};
    std::atomic<bool> activity_unfocused{false};
    std::atomic<bool> completed_unfocused{false};
    std::atomic<bool> last_turn_ok{true};

    // /find state: the active term and which hit (index into the recomputed
    // hit list) the viewport was last jumped to.  Guarded by layout_mu like
    // scroll_offset.
    std::string       find_term;
    int               find_idx            = -1;

    Pane*             parent_pane         = nullptr;
    std::string       spawn_message;
    std::string       last_response;
    std::atomic<bool> spawn_flowed{false};
};

} // namespace arbiter
