#include "tui/ansi_util.h"

namespace arbiter {

namespace {

bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

int utf8_code_unit_count(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t skip_escape(std::string_view raw, size_t i) {
    if (i + 1 >= raw.size()) return raw.size();
    const char next = raw[i + 1];
    if (next == '[') {
        size_t j = i + 2;
        while (j < raw.size() && static_cast<unsigned char>(raw[j]) < 0x40) ++j;
        if (j >= raw.size()) return raw.size();
        return j + 1;
    }
    if (next == ']') {
        size_t j = i + 2;
        while (j < raw.size() && raw[j] != 0x07 && raw[j] != 0x1B) ++j;
        if (j < raw.size() && raw[j] == 0x07) return j + 1;
        if (j + 1 < raw.size() && raw[j] == 0x1B && raw[j + 1] == '\\') return j + 2;
        return raw.size();
    }
    if (next == '7' || next == '8' || next == '=' || next == '>') return i + 2;
    return i + 1;
}

void peel_incomplete_utf8(std::string& text, std::string& hold) {
    if (text.empty()) return;

    size_t i = text.size();
    while (i > 0 && is_continuation(static_cast<unsigned char>(text[i - 1]))) --i;
    if (i == text.size()) return;

    const unsigned char lead = static_cast<unsigned char>(text[i]);
    const int need = utf8_code_unit_count(lead);
    if (need <= 1) return;

    const size_t have = text.size() - i;
    if (have >= static_cast<size_t>(need)) return;

    hold.insert(hold.end(), text.begin() + static_cast<std::ptrdiff_t>(i), text.end());
    text.resize(i);
}

std::string strip_complete(std::string_view raw, std::string& hold) {
    std::string out;
    out.reserve(raw.size());

    size_t i = 0;
    while (i < raw.size()) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1B) {
            const size_t end = skip_escape(raw, i);
            if (end >= raw.size()) {
                hold.assign(raw.data() + i, raw.size() - i);
                peel_incomplete_utf8(out, hold);
                return out;
            }
            i = end;
            continue;
        }
        if (c == 0x01 || c == 0x02) {
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }

    peel_incomplete_utf8(out, hold);
    return out;
}

} // namespace

std::string strip_ansi(std::string_view raw) {
    std::string hold;
    return strip_complete(raw, hold);
}

std::string AnsiStripStream::feed(std::string_view chunk) {
    std::string combined;
    combined.reserve(hold_.size() + chunk.size());
    combined.append(hold_);
    hold_.clear();
    combined.append(chunk);
    return strip_complete(combined, hold_);
}

std::string AnsiStripStream::flush() {
    if (hold_.empty()) return {};
    std::string tail = std::move(hold_);
    hold_.clear();
    return tail;
}

void AnsiStripStream::reset() {
    hold_.clear();
}

} // namespace arbiter
