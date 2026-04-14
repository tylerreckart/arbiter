#pragma once
// index/include/tui/line_editor.h
//
// Minimal in-house line editor.  We do this ourselves because libedit's
// interactive machinery is essentially undocumented — rl_getc_function is not
// called from its blocking readline(), interposing a PTY breaks the internal
// cursor-tracking state, and echo silently fails under our TUI layout.  The
// editor below handles what we actually need: printable inserts, backspace,
// arrow navigation, history, tab completion, and forwards mouse / page-scroll
// events to the caller.
//
// Threading model: read_line() is called on the main thread and blocks there
// reading stdin byte-by-byte (stdin is in raw mode).  It writes echo and
// redisplay directly to stdout under TUI::tty_mutex.  Scroll and cancel
// actions are surfaced through handler callbacks rather than internalised so
// the REPL (which owns the ScrollBuffer and the orchestrator) can respond.

#include "tui/tui.h"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace index_ai {

class LineEditor {
public:
    explicit LineEditor(TUI& tui);

    // Tab-completion callback.  Given the full current buffer and the token
    // under the cursor, returns candidate completions.  If the callback is
    // unset, Tab inserts a literal space.
    using CompletionFn = std::function<std::vector<std::string>(
        const std::string& buffer, const std::string& token)>;
    void set_completion_provider(CompletionFn fn) { completer_ = std::move(fn); }

    // History storage.  The editor never persists — the REPL is expected to
    // load from / save to disk via set_history() and history().
    void set_history(std::vector<std::string> h);
    const std::vector<std::string>& history() const { return history_; }
    void add_to_history(const std::string& line);
    void set_max_history(int n) { max_history_ = n; }

    // Mouse-wheel and PgUp/PgDn events.  direction: -1 = scroll up (toward
    // older content), +1 = scroll down.  step is measured in visual rows.
    using ScrollHandler = std::function<void(int direction, int step)>;
    void set_scroll_handler(ScrollHandler fn) { scroll_handler_ = std::move(fn); }

    // Invoked on lone ESC (no follow-on within 50 ms).  The handler is
    // expected to cancel any in-flight work and push an "[interrupted]"
    // notice into the scroll region.  The current input line is cleared.
    using CancelHandler = std::function<void()>;
    void set_cancel_handler(CancelHandler fn) { cancel_handler_ = std::move(fn); }

    // Blocks on stdin until the user submits the line (Enter) or EOF is
    // reached (Ctrl-D on empty line).  Returns false on EOF/interrupt —
    // `out` will be empty.
    bool read_line(const std::string& prompt, std::string& out);

    // Force a currently-running read_line() to return false.  Safe to call
    // from any thread.
    void interrupt();

private:
    // Helpers --------------------------------------------------------------
    int  read_byte();                 // blocking, -1 on interrupt / EOF
    bool read_byte_timed(int& out, int ms);   // returns false on timeout

    int  visible_width(std::string_view s) const;   // count printable cols (skip ANSI + UTF-8 cont.)

    void redraw();                    // redraw prompt+buffer from column 1
    void move_cursor_to_insertion();  // place cursor at buffer insertion point

    // Editing ops
    void insert_bytes(const char* data, size_t n);
    void backspace();
    void delete_char_at_cursor();
    void cursor_left();
    void cursor_right();
    void cursor_home();
    void cursor_end();
    void kill_to_end();
    void kill_whole_line();
    void kill_prev_word();
    void history_prev();
    void history_next();
    void tab_complete();

    // Sequence handling
    void handle_csi(char final, const std::string& params);

    // State ----------------------------------------------------------------
    TUI& tui_;
    CompletionFn    completer_;
    ScrollHandler   scroll_handler_;
    CancelHandler   cancel_handler_;
    std::vector<std::string> history_;
    int             max_history_ = 1000;

    // Per read_line() state
    std::string buffer_;
    int         cursor_      = 0;     // byte index into buffer_
    std::string prompt_;
    int         prompt_cols_ = 0;     // visible width of prompt_
    int         history_idx_ = -1;    // -1 = live buffer, >=0 = index in history_
    std::string saved_live_;          // buffer before entering history

    std::atomic<bool> interrupt_flag_{false};
};

} // namespace index_ai
