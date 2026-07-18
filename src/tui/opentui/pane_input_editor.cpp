#include "tui/opentui/pane_input_editor.h"

#include "tui/opentui/engine.h"
#include "tui/opentui/kitty_key_decode.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/select.h>
#include <unistd.h>

namespace arbiter::opentui {

namespace {

constexpr int kInputAccentCells = 1;

constexpr std::uint8_t kWrapWord = 2;

constexpr std::uint32_t kAttrBold = 1u << 0;

void draw_plain_text(OpenTuiHandle frame,
                     std::uint32_t x,
                     std::uint32_t y,
                     std::string_view text,
                     const TuiRgba& fg,
                     const TuiRgba* bg = nullptr,
                     std::uint32_t attrs = 0) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg ? bg->data() : nullptr,
                   attrs);
}

bool cursor_visible_now() {
    using clock = std::chrono::steady_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch()).count();
    return (ms / 500) % 2 == 0;
}

} // namespace

PaneInputEditor::PaneInputEditor(TUI& tui) : tui_(tui) {
    edit_ = createEditBuffer(/*wcwidth=*/0, /*event_sink=*/0);
    if (edit_ == 0) throw std::runtime_error("createEditBuffer failed");
    view_ = createEditorView(edit_, 80, 1);
    if (view_ == 0) {
        destroyEditBuffer(edit_);
        edit_ = 0;
        throw std::runtime_error("createEditorView failed");
    }
    editorViewSetWrapMode(view_, kWrapWord);
}

PaneInputEditor::~PaneInputEditor() {
    if (view_ != 0) destroyEditorView(view_);
    if (edit_ != 0) destroyEditBuffer(edit_);
}

void PaneInputEditor::set_shared_history(std::shared_ptr<SharedInputHistory> h) {
    std::lock_guard<std::mutex> lk(mu_);
    if (h) history_ = std::move(h);
}

void PaneInputEditor::set_history(std::vector<std::string> h) {
    history_->replace(std::move(h));
}

void PaneInputEditor::interrupt() {
    interrupt_flag_ = true;
}

bool PaneInputEditor::take_chord(char& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_chord_ == 0) return false;
    out = pending_chord_;
    pending_chord_ = 0;
    return true;
}

int PaneInputEditor::read_byte() {
    while (!interrupt_flag_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 100000};
        int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        unsigned char c;
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0) return -1;
        return c;
    }
    return -1;
}

bool PaneInputEditor::read_byte_timed(int& out, int ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) return false;
    unsigned char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return false;
    out = c;
    return true;
}

void PaneInputEditor::discard_osc() {
    while (!interrupt_flag_.load()) {
        int b = 0;
        if (!read_byte_timed(b, 100)) break;
        if (b == 0x07 || b == 0x9C) break;
        if (b == 0x1B) {
            int b2 = 0;
            if (read_byte_timed(b2, 10) && b2 == '\\') break;
        }
    }
}

void PaneInputEditor::discard_string_terminated() {
    while (!interrupt_flag_.load()) {
        int b = 0;
        if (!read_byte_timed(b, 100)) break;
        if (b == 0x9C) break;
        if (b == 0x1B) {
            int b2 = 0;
            if (read_byte_timed(b2, 10) && b2 == '\\') break;
        }
    }
}

bool PaneInputEditor::is_terminal_response_csi(const std::string& params, char final) {
    if (final == 'R') return true;
    if (final == 'c' && !params.empty()
        && (params[0] == '?' || params[0] == '>' || params[0] == '=')) {
        return true;
    }
    if (final == 'y' && params.find('$') != std::string::npos) return true;
    if (final == 'u' && !params.empty() && params[0] == '?') return true;
    return false;
}

int PaneInputEditor::read_key_event() {
    while (!interrupt_flag_.load()) {
        int b = read_byte();
        if (b < 0) return -1;
        if (b != 0x1B) return b;

        int b2 = 0;
        if (!read_byte_timed(b2, 50)) return 0x1B;

        if (b2 == '[') {
            std::string params;
            char final = 0;
            while (true) {
                int b3 = 0;
                if (!read_byte_timed(b3, 50)) break;
                // ANSI parameter bytes span 0x30-0x3F ('0'-'9', ':', ';',
                // '<', '=', '>', '?'). Kitty CSI-u reports use ':' inside
                // both the codepoint field (shifted/base-layout alternates,
                // sent because OpenTUI's pushed flags include "report
                // alternate keys") and the modifier field (event type) —
                // dropping it here means the whole sequence never reaches
                // decode_kitty_csi_u below, silently eating every kitty
                // key report that carries an alternate-key or event-type
                // subfield (i.e. most of them).
                if ((b3 >= '0' && b3 <= '9') || b3 == ':' || b3 == ';' || b3 == '?'
                    || b3 == '$' || b3 == '>' || b3 == '=' || b3 == '<') {
                    params += static_cast<char>(b3);
                    continue;
                }
                if (b3 >= 0x40 && b3 <= 0x7E) {
                    final = static_cast<char>(b3);
                    break;
                }
                break;
            }
            if (final) {
                if (is_terminal_response_csi(params, final)) continue;
                if (final == 'u') {
                    // Kitty-protocol key report — decode back to the legacy
                    // byte so it flows through the same dispatch as a
                    // directly-typed control code, regardless of whether the
                    // terminal ever honors our attempts to turn the protocol
                    // off (see kitty_key_decode.h).
                    if (auto legacy = decode_kitty_csi_u(params)) return *legacy;
                    continue;
                }
                std::lock_guard<std::mutex> lk(mu_);
                handle_csi(final, params);
            }
            continue;
        }

        if (b2 == ']') {
            discard_osc();
            continue;
        }

        if (b2 == 'P') {
            discard_string_terminated();
            continue;
        }

        if (b2 == 'O') {
            int b3 = 0;
            if (read_byte_timed(b3, 50)) {
                const char final = static_cast<char>(b3);
                if (final == 'A' || final == 'B' || final == 'C' || final == 'D'
                    || final == 'H' || final == 'F') {
                    std::lock_guard<std::mutex> lk(mu_);
                    handle_csi(final, "");
                    continue;
                }
            }
            continue;
        }

        continue;
    }
    return -1;
}

int PaneInputEditor::visible_width(std::string_view s) const {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x01) {
            ++i;
            while (i < s.size() && static_cast<unsigned char>(s[i]) != 0x02) ++i;
            if (i < s.size()) ++i;
            continue;
        }
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size()) {
                unsigned char x = static_cast<unsigned char>(s[i++]);
                if (x >= 0x40 && x <= 0x7E) break;
            }
            continue;
        }
        if ((c & 0xC0) == 0x80) {
            ++i;
            continue;
        }
        ++w;
        ++i;
    }
    return w;
}

std::string PaneInputEditor::plain_text(std::string_view s) const {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x01) {
            ++i;
            while (i < s.size() && static_cast<unsigned char>(s[i]) != 0x02) ++i;
            if (i < s.size()) ++i;
            continue;
        }
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size()) {
                unsigned char x = static_cast<unsigned char>(s[i++]);
                if (x >= 0x40 && x <= 0x7E) break;
            }
            continue;
        }
        out.push_back(static_cast<char>(s[i++]));
    }
    return out;
}

void PaneInputEditor::sync_edit_buffer() {
    if (buffer_.empty()) {
        editBufferClear(edit_);
        return;
    }
    editBufferSetText(edit_,
                      buffer_.data(),
                      static_cast<std::uint32_t>(buffer_.size()));
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_));
}

void PaneInputEditor::update_input_rows() {
    int cols = tui_.cols();
    if (cols <= 0) cols = 80;
    const TuiDesign& d = tui_design();
    const int raw_outer = (cols <= d.layout.dense_cols) ? 0 : std::max(0, d.layout.pane_padding_x);
    const int outer_pad = std::min(raw_outer, std::max(0, (cols - 1) / 2));
    const int raw_inner = (cols <= d.layout.dense_cols) ? 0 : std::max(0, d.layout.input_padding_x);
    const int inner_pad = std::min(raw_inner, std::max(0, (cols - outer_pad * 2 - 1) / 2));
    const int chrome_inset = kInputAccentCells + std::max(0, d.layout.header_padding_x);
    const int content_cols = std::max(1, cols - (outer_pad * 2) - chrome_inset);
    const int total_vis = prompt_cols_ + visible_width(buffer_) + (inner_pad * 2);
    const int editor_rows = std::max(1, (total_vis + content_cols - 1) / content_cols);
    tui_.grow_input(editor_rows + 2);
}

void PaneInputEditor::request_present() {
    if (present_fn_) present_fn_();
}

void PaneInputEditor::bind_viewport(const TUI& tui, int content_width) const {
    const int rows = std::max(1, tui.input_rows() - 2);
    const std::uint32_t w = static_cast<std::uint32_t>(std::max(1, content_width));
    const std::uint32_t h = static_cast<std::uint32_t>(rows);
    editorViewSetViewportSize(view_, w, h);
    // Viewport x/y are scroll offsets into virtual content, not screen position.
    editorViewSetViewport(view_, 0, 0, w, h, false);
}

void PaneInputEditor::draw(OpenTuiHandle frame, const TUI& tui, bool focused) const {
    if (frame == 0) return;
    std::lock_guard<std::mutex> lk(mu_);

    const std::uint32_t px = static_cast<std::uint32_t>(tui.left_col() - 1);
    const std::uint32_t py = static_cast<std::uint32_t>(tui.input_top_row_pub());
    const TuiDesign& d = tui_design();
    const int cols = std::max(1, tui.cols());
    const int raw_outer = (cols <= d.layout.dense_cols) ? 0 : std::max(0, d.layout.pane_padding_x);
    const int outer_pad = std::min(raw_outer, std::max(0, (cols - 1) / 2));
    const int raw_inner = (cols <= d.layout.dense_cols) ? 0 : std::max(0, d.layout.input_padding_x);
    const int inner_pad = std::min(raw_inner, std::max(0, (cols - outer_pad * 2 - 1) / 2));
    const int chrome_inset = kInputAccentCells + std::max(0, d.layout.header_padding_x);
    const std::uint32_t content_x = px
        + static_cast<std::uint32_t>(outer_pad + chrome_inset + inner_pad);
    const TuiRgba& input_bg = d.bg.header;

    if (!focused) {
        draw_plain_text(frame, content_x, py, d.component.inactive_prompt, d.text.subtle, &input_bg);
        return;
    }

    const int prompt_skip = std::max(0, prompt_cols_);
    const int editor_w = std::max(1,
                                  cols - (outer_pad * 2) - chrome_inset
                                      - (inner_pad * 2) - prompt_skip);
    const std::uint32_t ex = content_x + static_cast<std::uint32_t>(prompt_skip);

    bind_viewport(tui, editor_w);

    bufferDrawEditorView(frame, view_,
                         static_cast<int32_t>(ex),
                         static_cast<int32_t>(py));

    const std::string plain_prompt = plain_text(prompt_);
    if (!plain_prompt.empty()) {
        draw_plain_text(frame, content_x, py, plain_prompt, d.accent.primary, &input_bg);
    }
    if (cursor_visible_now()) {
        const int before_cursor = visible_width(buffer_.substr(0, static_cast<size_t>(cursor_)));
        const int cursor_row = std::max(0, before_cursor / std::max(1, editor_w));
        const int cursor_col = std::max(0, before_cursor % std::max(1, editor_w));
        const int max_rows = std::max(1, tui.input_rows() - 2);
        if (cursor_row < max_rows) {
            std::string cursor_cell = " ";
            if (cursor_ < static_cast<int>(buffer_.size())) {
                size_t end = static_cast<size_t>(cursor_ + 1);
                while (end < buffer_.size()
                       && (static_cast<unsigned char>(buffer_[end]) & 0xC0) == 0x80) {
                    ++end;
                }
                cursor_cell = buffer_.substr(static_cast<size_t>(cursor_), end - static_cast<size_t>(cursor_));
            }
            draw_plain_text(frame,
                            ex + static_cast<std::uint32_t>(cursor_col),
                            py + static_cast<std::uint32_t>(cursor_row),
                            cursor_cell,
                            d.text.inverse,
                            &d.accent.primary,
                            kAttrBold);
        }
    }

    // Command palette overlays the rows above the input strip; drawn last
    // so it paints over scroll content.
    if (palette_active_) draw_palette(frame, tui);
}

void PaneInputEditor::move_cursor_to_insertion() {
    (void)cursor_;
    request_present();
}

void PaneInputEditor::insert_bytes(const char* data, size_t n) {
    const std::uint32_t at = static_cast<std::uint32_t>(cursor_);
    buffer_.insert(static_cast<size_t>(cursor_), data, n);
    cursor_ += static_cast<int>(n);
    update_input_rows();
    editBufferSetCursorByOffset(edit_, at);
    editBufferInsertText(edit_, data, static_cast<std::uint32_t>(n));
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_));
    request_present();
}

void PaneInputEditor::backspace() {
    if (cursor_ == 0) return;
    int back = 1;
    while (back < cursor_
           && (static_cast<unsigned char>(buffer_[cursor_ - back]) & 0xC0) == 0x80) {
        ++back;
    }
    buffer_.erase(static_cast<size_t>(cursor_ - back), static_cast<size_t>(back));
    cursor_ -= back;
    update_input_rows();
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_ + back));
    editBufferDeleteCharBackward(edit_);
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_));
    request_present();
}

void PaneInputEditor::delete_char_at_cursor() {
    if (cursor_ >= static_cast<int>(buffer_.size())) return;
    int forward = 1;
    while (cursor_ + forward < static_cast<int>(buffer_.size())
           && (static_cast<unsigned char>(buffer_[cursor_ + forward]) & 0xC0) == 0x80) {
        ++forward;
    }
    buffer_.erase(static_cast<size_t>(cursor_), static_cast<size_t>(forward));
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

void PaneInputEditor::cursor_left() {
    if (cursor_ == 0) return;
    int back = 1;
    while (back < cursor_
           && (static_cast<unsigned char>(buffer_[cursor_ - back]) & 0xC0) == 0x80) {
        ++back;
    }
    cursor_ -= back;
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_));
    request_present();
}

void PaneInputEditor::cursor_right() {
    if (cursor_ >= static_cast<int>(buffer_.size())) return;
    int forward = 1;
    while (cursor_ + forward < static_cast<int>(buffer_.size())
           && (static_cast<unsigned char>(buffer_[cursor_ + forward]) & 0xC0) == 0x80) {
        ++forward;
    }
    cursor_ += forward;
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_));
    request_present();
}

void PaneInputEditor::cursor_home() {
    cursor_ = 0;
    editBufferSetCursorByOffset(edit_, 0);
    request_present();
}

void PaneInputEditor::cursor_end() {
    cursor_ = static_cast<int>(buffer_.size());
    editBufferSetCursorByOffset(edit_, static_cast<std::uint32_t>(cursor_));
    request_present();
}

void PaneInputEditor::kill_to_end() {
    if (cursor_ >= static_cast<int>(buffer_.size())) return;
    buffer_.erase(static_cast<size_t>(cursor_));
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

void PaneInputEditor::kill_whole_line() {
    buffer_.clear();
    cursor_ = 0;
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

void PaneInputEditor::kill_prev_word() {
    if (cursor_ == 0) return;
    int end = cursor_;
    while (end > 0 && buffer_[static_cast<size_t>(end - 1)] == ' ') --end;
    while (end > 0 && buffer_[static_cast<size_t>(end - 1)] != ' ') --end;
    buffer_.erase(static_cast<size_t>(end), static_cast<size_t>(cursor_ - end));
    cursor_ = end;
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

void PaneInputEditor::history_prev() {
    const int n = history_->size();
    if (n == 0) return;
    if (history_idx_ == -1) {
        saved_live_ = buffer_;
        history_idx_ = n - 1;
    } else if (history_idx_ > 0) {
        --history_idx_;
    } else {
        return;
    }
    buffer_ = history_->at(history_idx_);
    cursor_ = static_cast<int>(buffer_.size());
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

void PaneInputEditor::history_next() {
    if (history_idx_ == -1) return;
    ++history_idx_;
    if (history_idx_ >= history_->size()) {
        history_idx_ = -1;
        buffer_ = saved_live_;
    } else {
        buffer_ = history_->at(history_idx_);
    }
    cursor_ = static_cast<int>(buffer_.size());
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

// ─── Reverse incremental search (Ctrl-R) ────────────────────────────────────

void PaneInputEditor::rsearch_refresh() {
    // Re-run the query from the current position (inclusive) so a growing
    // query keeps the same match while it still matches, like readline.
    const int n = history_->size();
    const int from = rsearch_idx_ >= 0 ? rsearch_idx_ : n - 1;
    const int found = history_->rfind(rsearch_query_, from);
    if (found >= 0) {
        rsearch_idx_ = found;
        buffer_ = history_->at(found);
        cursor_ = static_cast<int>(buffer_.size());
    }
    // No match: keep the previous buffer; the "failed" prefix in the
    // prompt tells the user the trailing keystroke found nothing.
    const bool failed = !rsearch_query_.empty() && found < 0;
    prompt_ = std::string(failed ? "(failed reverse-i-search)`"
                                 : "(reverse-i-search)`")
            + rsearch_query_ + "': ";
    prompt_cols_ = visible_width(prompt_);
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

void PaneInputEditor::rsearch_begin() {
    if (rsearch_active_) return;
    rsearch_active_ = true;
    rsearch_query_.clear();
    rsearch_idx_ = -1;
    rsearch_saved_prompt_ = prompt_;
    rsearch_saved_prompt_cols_ = prompt_cols_;
    rsearch_saved_live_ = buffer_;
    rsearch_refresh();
}

void PaneInputEditor::rsearch_feed(char c) {
    rsearch_query_ += c;
    rsearch_refresh();
}

void PaneInputEditor::rsearch_backspace() {
    if (rsearch_query_.empty()) return;
    rsearch_query_.pop_back();
    // Widening the query may re-match from the newest entry again.
    rsearch_idx_ = -1;
    rsearch_refresh();
}

void PaneInputEditor::rsearch_cycle() {
    if (rsearch_query_.empty()) return;
    const int from = rsearch_idx_ > 0 ? rsearch_idx_ - 1 : -1;
    if (from < 0) return;   // already at the oldest match
    const int found = history_->rfind(rsearch_query_, from);
    if (found >= 0) rsearch_idx_ = found;
    rsearch_refresh();
}

void PaneInputEditor::rsearch_end(bool accept) {
    if (!rsearch_active_) return;
    rsearch_active_ = false;
    prompt_ = rsearch_saved_prompt_;
    prompt_cols_ = rsearch_saved_prompt_cols_;
    if (!accept) {
        buffer_ = rsearch_saved_live_;
        cursor_ = static_cast<int>(buffer_.size());
    }
    rsearch_query_.clear();
    rsearch_idx_ = -1;
    update_input_rows();
    sync_edit_buffer();
    request_present();
}

// ─── Command palette (Ctrl-P) ────────────────────────────────────────────────

void PaneInputEditor::set_palette_items(std::vector<arbiter::PaletteItem> items) {
    std::lock_guard<std::mutex> lk(mu_);
    palette_items_ = std::move(items);
}

void PaneInputEditor::palette_refresh() {
    palette_matches_ = arbiter::palette_filter(palette_items_, palette_query_);
    const int n = static_cast<int>(palette_matches_.size());
    palette_sel_ = std::clamp(palette_sel_, 0, std::max(0, n - 1));
    request_present();
}

void PaneInputEditor::palette_open() {
    if (palette_active_ || palette_items_.empty()) return;
    palette_active_ = true;
    palette_query_.clear();
    palette_sel_ = 0;
    palette_refresh();
}

void PaneInputEditor::palette_close(bool accept) {
    if (!palette_active_) return;
    palette_active_ = false;
    if (accept && palette_sel_ < static_cast<int>(palette_matches_.size())) {
        buffer_ = palette_matches_[static_cast<size_t>(palette_sel_)].name + " ";
        cursor_ = static_cast<int>(buffer_.size());
        update_input_rows();
        sync_edit_buffer();
    }
    palette_query_.clear();
    palette_matches_.clear();
    request_present();
}

void PaneInputEditor::draw_palette(OpenTuiHandle frame, const TUI& tui) const {
    constexpr int kMaxRows = 8;
    const TuiDesign& d = tui_design();
    const int cols = std::max(1, tui.cols());
    const int px = tui.left_col() - 1;
    const int input_top = tui.input_top_row_pub();

    const int n = static_cast<int>(palette_matches_.size());
    const int list_rows = std::min({kMaxRows, n, std::max(0, input_top - 2)});
    // +1 header row with the query.
    const int top = input_top - list_rows - 1;
    if (top < 1) return;

    const int w = std::min(cols, 64);
    const int x = px + 1;

    // Keep the selection inside the window.
    int first = 0;
    if (palette_sel_ >= list_rows) first = palette_sel_ - list_rows + 1;

    auto pad_to = [&](std::string s) {
        if (static_cast<int>(s.size()) > w) s.resize(static_cast<size_t>(w));
        s.resize(static_cast<size_t>(w), ' ');
        return s;
    };

    std::string header = " > " + palette_query_;
    if (n == 0) header += "   (no matching commands)";
    draw_plain_text(frame,
                    static_cast<std::uint32_t>(x),
                    static_cast<std::uint32_t>(top),
                    pad_to(header),
                    d.accent.primary,
                    &d.bg.header,
                    kAttrBold);

    for (int i = 0; i < list_rows; ++i) {
        const int idx = first + i;
        if (idx >= n) break;
        const auto& item = palette_matches_[static_cast<size_t>(idx)];
        std::string row = " " + item.name;
        if (!item.description.empty()) row += "  - " + item.description;
        const bool selected = idx == palette_sel_;
        draw_plain_text(frame,
                        static_cast<std::uint32_t>(x),
                        static_cast<std::uint32_t>(top + 1 + i),
                        pad_to(row),
                        selected ? d.text.inverse : d.text.primary,
                        selected ? &d.accent.primary : &d.bg.header,
                        selected ? kAttrBold : 0);
    }
}

void PaneInputEditor::tab_complete() {
    if (!completer_) return;
    int start = cursor_;
    while (start > 0 && buffer_[static_cast<size_t>(start - 1)] != ' ') --start;
    std::string token = buffer_.substr(static_cast<size_t>(start),
                                         static_cast<size_t>(cursor_ - start));
    auto matches = completer_(buffer_, token);
    if (matches.empty()) return;
    if (matches.size() == 1) {
        buffer_.replace(static_cast<size_t>(start),
                        static_cast<size_t>(cursor_ - start),
                        matches[0] + " ");
        cursor_ = start + static_cast<int>(matches[0].size()) + 1;
        update_input_rows();
        sync_edit_buffer();
        request_present();
        return;
    }
    std::string prefix = matches[0];
    for (size_t i = 1; i < matches.size() && !prefix.empty(); ++i) {
        size_t j = 0;
        while (j < prefix.size() && j < matches[i].size() && prefix[j] == matches[i][j]) ++j;
        prefix.resize(j);
    }
    if (prefix.size() > token.size()) {
        buffer_.replace(static_cast<size_t>(start),
                        static_cast<size_t>(cursor_ - start),
                        prefix);
        cursor_ = start + static_cast<int>(prefix.size());
        update_input_rows();
        sync_edit_buffer();
        request_present();
    }
}

void PaneInputEditor::handle_csi(char final, const std::string& params) {
    // Palette owns Up/Down while open; other nav keys are ignored so the
    // overlay doesn't fight the buffer underneath it.
    if (palette_active_) {
        if (params.empty() && final == 'A') {
            palette_sel_ = std::max(0, palette_sel_ - 1);
            request_present();
        } else if (params.empty() && final == 'B') {
            palette_sel_ = std::min(
                std::max(0, static_cast<int>(palette_matches_.size()) - 1),
                palette_sel_ + 1);
            request_present();
        }
        return;
    }
    // Arrow/nav keys during reverse-i-search accept the current match and
    // then apply normally (readline behavior).
    if (rsearch_active_) rsearch_end(true);
    if (params.empty()) {
        switch (final) {
            case 'A': history_prev(); return;
            case 'B': history_next(); return;
            case 'C': cursor_right(); return;
            case 'D': cursor_left();  return;
            case 'H': cursor_home();  return;
            case 'F': cursor_end();   return;
        }
    } else if (final == '~') {
        if (params == "1" || params == "7") { cursor_home(); return; }
        if (params == "3")                  { delete_char_at_cursor(); return; }
        if (params == "4" || params == "8") { cursor_end(); return; }
        if (params == "5" && scroll_handler_) {
            int step = std::max(1, tui_.scroll_region_rows() / 2);
            scroll_handler_(-1, step);
            return;
        }
        if (params == "6" && scroll_handler_) {
            int step = std::max(1, tui_.scroll_region_rows() / 2);
            scroll_handler_(+1, step);
            return;
        }
    }
}

bool PaneInputEditor::read_line(const std::string& prompt, std::string& out) {
    interrupt_flag_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        buffer_.clear();
        cursor_ = 0;
        prompt_ = prompt;
        prompt_cols_ = visible_width(prompt);
        history_idx_ = -1;
        saved_live_.clear();
        rsearch_active_ = false;
        palette_active_ = false;
        update_input_rows();
        sync_edit_buffer();
    }
    request_present();

    while (true) {
        int b = read_key_event();
        if (b < 0) {
            out.clear();
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (palette_active_) {
                if (b >= 0x20 && b != 0x7F) {
                    palette_query_ += static_cast<char>(b);
                    palette_refresh();
                    continue;
                }
                if (b == 0x7F || b == 0x08) {
                    if (!palette_query_.empty()) palette_query_.pop_back();
                    palette_refresh();
                    continue;
                }
                if (b == '\r' || b == '\n' || b == 0x09) {
                    palette_close(/*accept=*/true);
                    continue;
                }
                if (b == 0x1B || b == 0x07 || b == 0x10) {
                    palette_close(/*accept=*/false);
                    continue;
                }
                continue;   // swallow everything else while the overlay is up
            }
            if (rsearch_active_) {
                if (b >= 0x20 && b != 0x7F) {
                    rsearch_feed(static_cast<char>(b));
                    continue;
                }
                if (b == 0x12) { rsearch_cycle(); continue; }
                if (b == 0x7F || b == 0x08) { rsearch_backspace(); continue; }
                if (b == 0x1B || b == 0x07) { rsearch_end(false); continue; }
                // Enter (and any other control key) accepts the match and
                // then applies normally in the dispatch below.
                rsearch_end(true);
            }
        }

        if (b >= 0x20 && b != 0x7F) {
            char c = static_cast<char>(b);
            std::lock_guard<std::mutex> lk(mu_);
            insert_bytes(&c, 1);
            continue;
        }

        switch (b) {
            case '\r':
            case '\n': {
                std::lock_guard<std::mutex> lk(mu_);
                out = buffer_;
                buffer_.clear();
                cursor_ = 0;
                editBufferClear(edit_);
                return true;
            }
            case 0x7F:
            case 0x08: {
                std::lock_guard<std::mutex> lk(mu_);
                backspace();
                continue;
            }
            case 0x01: {
                std::lock_guard<std::mutex> lk(mu_);
                cursor_home();
                continue;
            }
            case 0x05: {
                std::lock_guard<std::mutex> lk(mu_);
                cursor_end();
                continue;
            }
            case 0x02: {
                std::lock_guard<std::mutex> lk(mu_);
                cursor_left();
                continue;
            }
            case 0x06: {
                std::lock_guard<std::mutex> lk(mu_);
                cursor_right();
                continue;
            }
            case 0x0B: {
                std::lock_guard<std::mutex> lk(mu_);
                kill_to_end();
                continue;
            }
            case 0x15: {
                std::lock_guard<std::mutex> lk(mu_);
                kill_whole_line();
                continue;
            }
            case 0x17:
                if (chord_handler_) {
                    int b2;
                    if (read_byte_timed(b2, 2000)) {
                        if (chord_handler_(static_cast<char>(b2))) {
                            std::lock_guard<std::mutex> lk(mu_);
                            pending_chord_ = static_cast<char>(b2);
                            out.clear();
                            return false;
                        }
                    }
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    kill_prev_word();
                }
                continue;
            case 0x0F: {
                if (code_expand_handler_) code_expand_handler_();
                continue;
            }
            case 0x12: {
                std::lock_guard<std::mutex> lk(mu_);
                rsearch_begin();
                continue;
            }
            case 0x10: {
                std::lock_guard<std::mutex> lk(mu_);
                palette_open();
                continue;
            }
            case 0x09: {
                std::lock_guard<std::mutex> lk(mu_);
                tab_complete();
                continue;
            }
            case 0x04: {
                std::lock_guard<std::mutex> lk(mu_);
                if (buffer_.empty()) {
                    out.clear();
                    return false;
                }
                delete_char_at_cursor();
                continue;
            }
            case 0x03: {
                if (cancel_handler_) cancel_handler_();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    buffer_.clear();
                    cursor_ = 0;
                    editBufferClear(edit_);
                }
                out.clear();
                request_present();
                return true;
            }
            case 0x1B: {
                if (cancel_handler_) cancel_handler_();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    buffer_.clear();
                    cursor_ = 0;
                    editBufferClear(edit_);
                }
                request_present();
                continue;
            }
            default:
                continue;
        }
    }
}

} // namespace arbiter::opentui
