#pragma once

#include "repl/queues.h"
#include "tui/line_editor.h"
#include "tui/tui.h"
#include "tui/opentui/pane_scroll_view.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace arbiter {

struct Pane {
    TUI               tui;
    ThinkingIndicator thinking{&tui};
    ToolCallIndicator tool_indicator{&tui};
    LineEditor        editor{tui};

    OutputQueue       output_queue;
    CommandQueue      cmd_queue;

    std::unique_ptr<opentui::PaneScrollView> scroll;

    std::thread       exec_thread;

    std::string       current_agent = "index";
    std::string       current_model;
    std::string       original_task;
    std::string       multiline_accum;
    int               scroll_offset       = 0;
    int               new_while_scrolled  = 0;
    bool              welcome_visible     = true;

    Pane*             parent_pane         = nullptr;
    std::string       spawn_message;
    std::string       last_response;
    std::atomic<bool> spawn_flowed{false};
};

} // namespace arbiter
