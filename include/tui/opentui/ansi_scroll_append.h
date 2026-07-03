#pragma once

#include "tui/opentui/c_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbiter::opentui {

// Incrementally append ANSI-styled text (from MarkdownRenderer / theme) into an
// OpenTUI TextBuffer, preserving SGR colors and attributes as highlights.
class AnsiScrollAppender {
public:
    explicit AnsiScrollAppender(OpenTuiHandle buffer);
    ~AnsiScrollAppender();

    AnsiScrollAppender(const AnsiScrollAppender&) = delete;
    AnsiScrollAppender& operator=(const AnsiScrollAppender&) = delete;

    void clear();
    void append(std::string_view raw);

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

    void feed_bytes(std::string_view chunk);
    void apply_sgr(const std::vector<int>& params);
    void emit_plain(std::string_view text);
    std::uint32_t style_id_for_current();
    static std::array<std::uint16_t, 4> pack_rgb(std::uint8_t r,
                                                  std::uint8_t g,
                                                  std::uint8_t b);

    OpenTuiHandle buffer_{0};
    OpenTuiHandle syntax_{0};
    std::string hold_;
    std::optional<std::array<std::uint16_t, 4>> fg_;
    std::optional<std::array<std::uint16_t, 4>> bg_;
    std::uint32_t attrs_ = 0;
    std::unordered_map<StyleKey, std::uint32_t, StyleKeyHash> style_cache_;
    std::uint32_t next_style_name_ = 0;
};

} // namespace arbiter::opentui
