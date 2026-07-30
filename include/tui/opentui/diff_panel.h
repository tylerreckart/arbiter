#pragma once

#include "tui/opentui/c_api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

// GitHub-style split (or unified on narrow terminals) diff panel rendered with
// OpenTUI fill/text primitives — box, line backgrounds, gutters, signs.
class DiffPanel {
public:
    void set_patch(std::string_view patch);
    [[nodiscard]] int visual_rows() const { return static_cast<int>(rows_); }
    // One plain-text string per visual row (header + body), matching the panel
    // layout used by draw() — for mouse selection copy.
    void collect_visual_lines(std::vector<std::string>& out) const;
    void draw(OpenTuiHandle frame,
              int x,
              int y,
              int w,
              int h,
              int skip_rows) const;

private:
    enum class Kind : std::uint8_t { context, add, remove, empty };

    struct SideLine {
        Kind kind = Kind::context;
        std::string content;
        std::uint32_t line_num = 0;
        bool show_line_number = false;
        char sign = '\0';
    };

    struct Row {
        SideLine left;
        SideLine right;
    };

    std::string header_old_;
    std::string header_new_;
    std::vector<Row> lines_;
    std::size_t rows_ = 0;
    bool split_ = true;
    // When !split_: add → show only right (new file); delete → only left.
    bool single_side_is_new_ = true;

    [[nodiscard]] int gutter_width() const;
    [[nodiscard]] static bool is_dev_null(std::string_view path);
};

} // namespace arbiter::opentui
