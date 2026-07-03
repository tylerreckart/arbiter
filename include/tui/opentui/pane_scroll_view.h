#pragma once

#include "tui/opentui/c_api.h"
#include "tui/ansi_util.h"
#include "tui/tui.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

class Engine;

class PaneScrollView {
public:
    PaneScrollView();
    ~PaneScrollView();

    PaneScrollView(const PaneScrollView&) = delete;
    PaneScrollView& operator=(const PaneScrollView&) = delete;

    void bind(const TUI& tui);
    void set_wrap_cols(int cols);

    void append(std::string_view text);
    void clear();

    [[nodiscard]] int total_visual_rows() const;
    [[nodiscard]] int max_scroll_offset() const;

    void draw(OpenTuiHandle frame,
              TUI& tui,
              int scroll_offset,
              int new_while_scrolled);

private:
    void sync_scroll_offset(int scroll_offset) const;

    OpenTuiHandle buffer_{0};
    OpenTuiHandle view_{0};
    int buf_x_{0};
    int buf_y_{0};
    int viewport_w_{0};
    int viewport_h_{0};
    int wrap_cols_{80};

    std::vector<std::string> text_storage_;
    AnsiStripStream strip_stream_;
};

} // namespace arbiter::opentui
