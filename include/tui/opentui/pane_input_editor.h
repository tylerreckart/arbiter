#pragma once

#include "tui/opentui/c_api.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/shared_input_history.h"
#include "tui/palette.h"
#include "tui/tui.h"

#include <atomic>
#include <functional>
#include <memory>
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

    // Ctrl-P command palette: a fuzzy-filtered overlay over these items.
    // Enter replaces the input buffer with the selection; Esc closes.
    void set_palette_items(std::vector<arbiter::PaletteItem> items);

    // Attach a history store shared with other editors — commands typed in
    // any pane become visible to every pane's Up-arrow / Ctrl-R instantly.
    void set_shared_history(std::shared_ptr<SharedInputHistory> h);
    // Convenience: replace the attached store's contents (standalone
    // editors and tests; the store stays whichever one is attached).
    void set_history(std::vector<std::string> h);
    std::vector<std::string> history() const { return history_->snapshot(); }
    void add_to_history(const std::string& line) { history_->add(line); }
    void set_max_history(int n) { history_->set_max(n); }

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

    // Mouse events decoded from SGR CSI reports. Return true to exit
    // read_line (e.g. focus moved to another pane); false to keep editing.
    using MouseHandler = std::function<bool(const MouseEvent& ev)>;
    void set_mouse_handler(MouseHandler fn) { mouse_handler_ = std::move(fn); }

    void set_present_fn(std::function<void()> fn) { present_fn_ = std::move(fn); }

    bool take_chord(char& out);

    bool read_line(const std::string& prompt, std::string& out);
    void interrupt();

    // Place the caret from a 0-based terminal click inside this pane's
    // input band. No-op when the click falls outside the editor cells.
    void set_cursor_from_click(int term_x, int term_y);

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

    // Ctrl-R reverse incremental search over the shared history.  All five
    // assume mu_ is held.  begin saves the live buffer/prompt; feed/backspace
    // edit the query and re-search; cycle steps to the next-older match;
    // end(true) keeps the matched text in the buffer (accept), end(false)
    // restores the pre-search state (cancel).
    void rsearch_begin();
    void rsearch_feed(char c);
    void rsearch_backspace();
    void rsearch_cycle();
    void rsearch_end(bool accept);
    void rsearch_refresh();   // re-run query, update buffer + prompt

    // Ctrl-P palette (all assume mu_ held).  accept=true replaces the
    // buffer with the selected item's name.
    void palette_open();
    void palette_close(bool accept);
    void palette_refresh();   // re-filter matches, clamp selection
    void draw_palette(OpenTuiHandle frame, const TUI& tui) const;

    int  read_key_event();
    bool read_bracketed_paste(std::string& out);
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
    MouseHandler    mouse_handler_;
    std::function<void()> present_fn_;

    char pending_chord_ = 0;
    std::shared_ptr<SharedInputHistory> history_ =
        std::make_shared<SharedInputHistory>();

    mutable std::mutex mu_;
    OpenTuiHandle edit_{0};
    OpenTuiHandle view_{0};

    std::string buffer_;
    std::string pending_paste_;
    int         cursor_      = 0;
    std::string prompt_;
    int         prompt_cols_ = 0;
    int         history_idx_ = -1;
    std::string saved_live_;

    // Reverse-i-search state (valid while rsearch_active_).
    bool        rsearch_active_ = false;
    std::string rsearch_query_;
    int         rsearch_idx_ = -1;          // current match; -1 = none
    std::string rsearch_saved_prompt_;
    int         rsearch_saved_prompt_cols_ = 0;
    std::string rsearch_saved_live_;

    // Command palette state (valid while palette_active_).
    bool        palette_active_ = false;
    std::string palette_query_;
    int         palette_sel_ = 0;           // index into palette_matches_
    std::vector<arbiter::PaletteItem> palette_items_;
    std::vector<arbiter::PaletteItem> palette_matches_;

    std::atomic<bool> interrupt_flag_{false};
};

} // namespace arbiter::opentui
