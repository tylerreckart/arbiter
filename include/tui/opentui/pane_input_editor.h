#pragma once

#include "tui/opentui/c_api.h"
#include "tui/tui.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

// OpenTUI EditBuffer + EditorView input for one pane.  Mirrors LineEditor's
// REPL-facing API; keystrokes are read on the main thread and painted by the
// output pump via bufferDrawEditorView (no ANSI echo to stdout).
class PaneInputEditor {
public:
    explicit PaneInputEditor(TUI& tui);
    ~PaneInputEditor();

    using CompletionFn = std::function<std::vector<std::string>(
        const std::string& buffer, const std::string& token)>;
    void set_completion_provider(CompletionFn fn) { completer_ = std::move(fn); }

    void set_history(std::vector<std::string> h);
    const std::vector<std::string>& history() const { return history_; }
    void add_to_history(const std::string& line);
    void set_max_history(int n) { max_history_ = n; }

    using ScrollHandler = std::function<void(int direction, int step)>;
    void set_scroll_handler(ScrollHandler fn) { scroll_handler_ = std::move(fn); }

    using CodeExpandHandler = std::function<void()>;
    void set_code_expand_handler(CodeExpandHandler fn) {
        code_expand_handler_ = std::move(fn);
    }

    using CancelHandler = std::function<void()>;
    void set_cancel_handler(CancelHandler fn) { cancel_handler_ = std::move(fn); }

    using ChordHandler = std::function<bool(char cmd)>;
    void set_chord_handler(ChordHandler fn) { chord_handler_ = std::move(fn); }

    void set_present_fn(std::function<void()> fn) { present_fn_ = std::move(fn); }

    bool take_chord(char& out);

    bool read_line(const std::string& prompt, std::string& out);
    void interrupt();

    void draw(OpenTuiHandle frame, const TUI& tui, bool focused) const;

private:
    int  read_byte();
    bool read_byte_timed(int& out, int ms);

    int  visible_width(std::string_view s) const;
    std::string plain_text(std::string_view s) const;

    void sync_edit_buffer();
    void update_input_rows();
    void request_present();
    void move_cursor_to_insertion();

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

    int  read_key_event();
    void discard_osc();
    void discard_string_terminated();
    static bool is_terminal_response_csi(const std::string& params, char final);

    void handle_csi(char final, const std::string& params);

    void bind_viewport(const TUI& tui, int content_width) const;

    TUI& tui_;
    CompletionFn    completer_;
    ScrollHandler   scroll_handler_;
    CodeExpandHandler code_expand_handler_;
    CancelHandler   cancel_handler_;
    ChordHandler    chord_handler_;
    std::function<void()> present_fn_;

    char pending_chord_ = 0;
    std::vector<std::string> history_;
    int  max_history_ = 1000;

    mutable std::mutex mu_;
    OpenTuiHandle edit_{0};
    OpenTuiHandle view_{0};

    std::string buffer_;
    int         cursor_      = 0;
    std::string prompt_;
    int         prompt_cols_ = 0;
    int         history_idx_ = -1;
    std::string saved_live_;

    std::atomic<bool> interrupt_flag_{false};
};

} // namespace arbiter::opentui
