#pragma once

#include "tui/opentui/c_api.h"

#include "styled_text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbiter::opentui {

// Append StyledLine spans directly into an OpenTUI TextBuffer — no ANSI round trip.
class SpanScrollAppender {
public:
    explicit SpanScrollAppender(OpenTuiHandle buffer);
    ~SpanScrollAppender();

    SpanScrollAppender(const SpanScrollAppender&) = delete;
    SpanScrollAppender& operator=(const SpanScrollAppender&) = delete;

    void clear();
    void append_line(const StyledLine& line);

private:
    struct StyleKey {
        std::array<std::uint16_t, 4> fg{};
        std::array<std::uint16_t, 4> bg{};
        std::uint32_t attrs = 0;
        bool has_fg = false;
        bool has_bg = false;

        bool operator==(const StyleKey& o) const;
    };

    struct StyleKeyHash {
        std::size_t operator()(const StyleKey& k) const;
    };

    struct StoredRun {
        std::string text;
        StyleKey style;
        bool has_style = false;
    };

    void emit_plain(std::string_view text, StyleId id);
    void add_highlight(std::uint32_t start, std::uint32_t end, const StyleKey& key);
    std::uint32_t style_id_for_key(const StyleKey& key);
    StyleKey style_key_for(StyleId id) const;
    void compact_storage_if_needed();
    void rebuild_buffer_from_storage();

    OpenTuiHandle buffer_{0};
    OpenTuiHandle syntax_{0};
    std::string utf8_hold_;
    std::deque<StoredRun> plain_storage_;
    std::unordered_map<StyleKey, std::uint32_t, StyleKeyHash> style_cache_;
    std::uint32_t next_style_name_ = 0;
    // Newlines are separators between lines, not terminators — a trailing `\n`
    // would create a phantom empty visual row and stack with block_gap blanks.
    bool has_line_ = false;
};

} // namespace arbiter::opentui
