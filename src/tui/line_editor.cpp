// index_ai/src/tui/line_editor.cpp — see tui/line_editor.h

#include "tui/line_editor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/select.h>
#include <unistd.h>

namespace index_ai {

LineEditor::LineEditor(TUI& tui) : tui_(tui) {}

void LineEditor::set_history(std::vector<std::string> h) {
    history_ = std::move(h);
    while ((int)history_.size() > max_history_)
        history_.erase(history_.begin());
}

void LineEditor::add_to_history(const std::string& line) {
    if (line.empty()) return;
    if (!history_.empty() && history_.back() == line) return;  // dedupe consecutive
    history_.push_back(line);
    while ((int)history_.size() > max_history_)
        history_.erase(history_.begin());
}

void LineEditor::interrupt() {
    interrupt_flag_ = true;
}

// ─── low-level input ────────────────────────────────────────────────────────

int LineEditor::read_byte() {
    while (!interrupt_flag_.load()) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 100000};   // 100 ms — lets us notice interrupt_flag_
        int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        unsigned char c;
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0) return -1;
        return c;
    }
    return -1;
}

bool LineEditor::read_byte_timed(int& out, int ms) {
    fd_set rfds; FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) return false;
    unsigned char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return false;
    out = c;
    return true;
}

// Count visible columns: skip CSI escape sequences and \001/\002 markers,
// count one column per non-continuation UTF-8 byte.
int LineEditor::visible_width(std::string_view s) const {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        // \001 / \002 markers wrap invisible runs (ANSI etc.).
        if (c == 0x01) {
            ++i;
            while (i < s.size() && (unsigned char)s[i] != 0x02) ++i;
            if (i < s.size()) ++i;  // skip the \002
            continue;
        }
        // Raw CSI: ESC [ ... final-byte
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size()) {
                unsigned char x = (unsigned char)s[i++];
                if (x >= 0x40 && x <= 0x7E) break;
            }
            continue;
        }
        // UTF-8 continuation
        if ((c & 0xC0) == 0x80) { ++i; continue; }
        ++w;
        ++i;
    }
    return w;
}

// ─── redisplay ─────────────────────────────────────────────────────────────

void LineEditor::redraw() {
    std::lock_guard<std::recursive_mutex> lk(tui_.tty_mutex());

    // Wrap-aware redraw.  The naive "\r\033[K" approach only clears the
    // current row, so once the buffer wrapped to a second line, old wrapped
    // text ghosted below the new prompt.  Instead: expand the input area to
    // fit the wrapped content, clear every row in it, reprint from the top,
    // then place the cursor using absolute row/col arithmetic.
    int cols = tui_.cols();
    if (cols <= 0) cols = 80;

    int total_vis   = prompt_cols_ + visible_width(buffer_);
    // +1 row so the cursor has somewhere to sit when the buffer fills an
    // entire row exactly (cursor-at-end wraps to the next line).
    int needed_rows = std::max(1, (total_vis + cols) / cols);
    tui_.grow_input(needed_rows);

    int top = tui_.input_top_row_pub();
    int bot = tui_.input_bottom_row_pub();
    for (int r = top; r <= bot; ++r)
        std::printf("\033[%d;1H\033[2K", r);

    std::printf("\033[%d;1H", top);
    std::fwrite(prompt_.data(), 1, prompt_.size(), stdout);
    std::fwrite(buffer_.data(), 1, buffer_.size(), stdout);

    int cursor_vis = prompt_cols_ + visible_width(std::string_view(buffer_).substr(0, cursor_));
    int cursor_row = top + (cursor_vis / cols);
    int cursor_col = (cursor_vis % cols) + 1;
    std::printf("\033[%d;%dH", cursor_row, cursor_col);

    std::fflush(stdout);
}

void LineEditor::move_cursor_to_insertion() {
    // No redraw — just place the cursor at the right wrapped row/col.
    std::lock_guard<std::recursive_mutex> lk(tui_.tty_mutex());
    int cols = tui_.cols();
    if (cols <= 0) cols = 80;
    int top = tui_.input_top_row_pub();
    int cursor_vis = prompt_cols_ + visible_width(std::string_view(buffer_).substr(0, cursor_));
    int cursor_row = top + (cursor_vis / cols);
    int cursor_col = (cursor_vis % cols) + 1;
    std::printf("\033[%d;%dH", cursor_row, cursor_col);
    std::fflush(stdout);
}

// ─── editing ops ───────────────────────────────────────────────────────────

void LineEditor::insert_bytes(const char* data, size_t n) {
    buffer_.insert(cursor_, data, n);
    cursor_ += (int)n;
    redraw();
}

void LineEditor::backspace() {
    if (cursor_ == 0) return;
    // Walk back one UTF-8 code point.
    int back = 1;
    while (back < cursor_ && ((unsigned char)buffer_[cursor_ - back] & 0xC0) == 0x80) ++back;
    buffer_.erase(cursor_ - back, back);
    cursor_ -= back;
    redraw();
}

void LineEditor::delete_char_at_cursor() {
    if (cursor_ >= (int)buffer_.size()) return;
    int forward = 1;
    while (cursor_ + forward < (int)buffer_.size()
           && ((unsigned char)buffer_[cursor_ + forward] & 0xC0) == 0x80) ++forward;
    buffer_.erase(cursor_, forward);
    redraw();
}

void LineEditor::cursor_left() {
    if (cursor_ == 0) return;
    int back = 1;
    while (back < cursor_ && ((unsigned char)buffer_[cursor_ - back] & 0xC0) == 0x80) ++back;
    cursor_ -= back;
    move_cursor_to_insertion();
}

void LineEditor::cursor_right() {
    if (cursor_ >= (int)buffer_.size()) return;
    int forward = 1;
    while (cursor_ + forward < (int)buffer_.size()
           && ((unsigned char)buffer_[cursor_ + forward] & 0xC0) == 0x80) ++forward;
    cursor_ += forward;
    move_cursor_to_insertion();
}

void LineEditor::cursor_home() { cursor_ = 0; move_cursor_to_insertion(); }
void LineEditor::cursor_end()  { cursor_ = (int)buffer_.size(); move_cursor_to_insertion(); }

void LineEditor::kill_to_end() {
    if (cursor_ >= (int)buffer_.size()) return;
    buffer_.erase(cursor_);
    redraw();
}

void LineEditor::kill_whole_line() {
    buffer_.clear();
    cursor_ = 0;
    redraw();
}

void LineEditor::kill_prev_word() {
    if (cursor_ == 0) return;
    int end = cursor_;
    // Skip trailing spaces
    while (end > 0 && buffer_[end - 1] == ' ') --end;
    // Then skip word chars
    while (end > 0 && buffer_[end - 1] != ' ') --end;
    buffer_.erase(end, cursor_ - end);
    cursor_ = end;
    redraw();
}

void LineEditor::history_prev() {
    if (history_.empty()) return;
    if (history_idx_ == -1) {
        saved_live_ = buffer_;
        history_idx_ = (int)history_.size() - 1;
    } else if (history_idx_ > 0) {
        --history_idx_;
    } else {
        return;
    }
    buffer_ = history_[history_idx_];
    cursor_ = (int)buffer_.size();
    redraw();
}

void LineEditor::history_next() {
    if (history_idx_ == -1) return;
    ++history_idx_;
    if (history_idx_ >= (int)history_.size()) {
        history_idx_ = -1;
        buffer_ = saved_live_;
    } else {
        buffer_ = history_[history_idx_];
    }
    cursor_ = (int)buffer_.size();
    redraw();
}

void LineEditor::tab_complete() {
    if (!completer_) return;
    // Extract the token preceding the cursor (run of non-space bytes).
    int start = cursor_;
    while (start > 0 && buffer_[start - 1] != ' ') --start;
    std::string token = buffer_.substr(start, cursor_ - start);
    auto matches = completer_(buffer_, token);
    if (matches.empty()) return;
    // Simple behaviour: if exactly one match, replace the token.
    if (matches.size() == 1) {
        buffer_.replace(start, cursor_ - start, matches[0] + " ");
        cursor_ = start + (int)matches[0].size() + 1;
        redraw();
        return;
    }
    // Multiple matches: replace with the longest common prefix (if it extends
    // the current token) and leave the rest to the user.
    std::string prefix = matches[0];
    for (size_t i = 1; i < matches.size() && !prefix.empty(); ++i) {
        size_t j = 0;
        while (j < prefix.size() && j < matches[i].size() && prefix[j] == matches[i][j]) ++j;
        prefix.resize(j);
    }
    if (prefix.size() > token.size()) {
        buffer_.replace(start, cursor_ - start, prefix);
        cursor_ = start + (int)prefix.size();
        redraw();
    }
}

// ─── CSI dispatch ──────────────────────────────────────────────────────────

void LineEditor::handle_csi(char final, const std::string& params) {
    if (params.empty()) {
        switch (final) {
            case 'A': history_prev();  return;
            case 'B': history_next();  return;
            case 'C': cursor_right();  return;
            case 'D': cursor_left();   return;
            case 'H': cursor_home();   return;
            case 'F': cursor_end();    return;
        }
    } else if (final == '~') {
        // \033[Nn~ sequences: 1=Home, 3=Delete, 4=End, 5=PgUp, 6=PgDn, etc.
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
    // Unknown sequence — silently ignored.
}

// ─── main loop ─────────────────────────────────────────────────────────────

bool LineEditor::read_line(const std::string& prompt, std::string& out) {
    interrupt_flag_ = false;
    buffer_.clear();
    cursor_ = 0;
    prompt_ = prompt;
    prompt_cols_ = visible_width(prompt);
    history_idx_ = -1;
    saved_live_.clear();

    // Paint the initial prompt.
    redraw();

    while (true) {
        int b = read_byte();
        if (b < 0) { out.clear(); return false; }

        // ── printable / UTF-8 ────────────────────────────────────────────
        if (b >= 0x20 && b != 0x7F) {
            char c = (char)b;
            insert_bytes(&c, 1);
            continue;
        }

        // ── control keys ─────────────────────────────────────────────────
        switch (b) {
            case '\r':
            case '\n': {
                // Echo a newline so any subsequent stdout writes start fresh.
                {
                    std::lock_guard<std::recursive_mutex> lk(tui_.tty_mutex());
                    std::fputs("\r\n", stdout);
                    std::fflush(stdout);
                }
                out = buffer_;
                return true;
            }
            case 0x7F:           // Backspace (DEL on most terminals)
            case 0x08:           // ^H
                backspace();     continue;
            case 0x01: cursor_home();      continue;  // ^A
            case 0x05: cursor_end();       continue;  // ^E
            case 0x02: cursor_left();      continue;  // ^B
            case 0x06: cursor_right();     continue;  // ^F
            case 0x0B: kill_to_end();      continue;  // ^K
            case 0x15: kill_whole_line();  continue;  // ^U
            case 0x17: kill_prev_word();   continue;  // ^W
            case 0x09: tab_complete();     continue;  // Tab

            case 0x04:           // ^D  — EOF on empty line, else delete-forward
                if (buffer_.empty()) { out.clear(); return false; }
                delete_char_at_cursor();
                continue;

            case 0x03:           // ^C — abort current line
                if (cancel_handler_) cancel_handler_();
                buffer_.clear();
                cursor_ = 0;
                {
                    std::lock_guard<std::recursive_mutex> lk(tui_.tty_mutex());
                    std::fputs("\r\n", stdout);
                    std::fflush(stdout);
                }
                out.clear();
                return true;   // "empty line submitted"

            case 0x1B: {         // ESC — possibly start of a sequence
                int b2;
                if (!read_byte_timed(b2, 50)) {
                    // Lone ESC → cancel
                    if (cancel_handler_) cancel_handler_();
                    buffer_.clear();
                    cursor_ = 0;
                    redraw();
                    continue;
                }
                if (b2 != '[') {
                    // Alt-<key> — ignored for now.
                    continue;
                }
                // CSI: collect parameters (digits and ';') until a final byte.
                std::string params;
                char final = 0;
                while (true) {
                    int b3;
                    if (!read_byte_timed(b3, 50)) break;
                    if (b3 == 'M') {
                        // X10 mouse: 3 more bytes follow (btn, col, row).
                        int bb, xx, yy;
                        if (!read_byte_timed(bb, 50)) break;
                        if (!read_byte_timed(xx, 50)) break;
                        if (!read_byte_timed(yy, 50)) break;
                        int btn = bb - 32;
                        if ((btn & 64) && scroll_handler_) {
                            int dir = (btn & 1) ? +1 : -1;
                            scroll_handler_(dir, 3);
                        }
                        break;
                    }
                    if ((b3 >= '0' && b3 <= '9') || b3 == ';') {
                        params += (char)b3;
                        continue;
                    }
                    final = (char)b3;
                    break;
                }
                if (final) handle_csi(final, params);
                continue;
            }
            default:
                // Unknown control byte — drop silently.
                continue;
        }
    }
}

} // namespace index_ai
