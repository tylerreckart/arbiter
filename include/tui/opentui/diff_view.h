#pragma once

#include "tui/opentui/c_api.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace arbiter::opentui {

// Thin C++ wrapper around the native DiffView handle exposed by OpenTUI's C ABI.
class DiffView {
public:
    struct Options {
        uint8_t view_mode = 0;   // 0=unified, 1=split
        uint8_t wrap_mode = 0;   // 0=none, 1=char, 2=word
        bool show_line_numbers = true;
    };

    DiffView();
    explicit DiffView(const Options& opts);
    ~DiffView();

    DiffView(const DiffView&) = delete;
    DiffView& operator=(const DiffView&) = delete;

    bool set_patch(std::string_view patch);
    bool set_view_mode(uint8_t mode);
    void set_wrap_mode(uint8_t mode);
    void set_wrap_width(std::uint32_t content_width);
    void set_scroll_y(std::uint32_t offset);

    std::uint32_t virtual_line_count() const;
    std::uint32_t hunk_count() const;
    std::uint32_t hunk_start_line(std::uint32_t hunk_index) const;

    void draw(OpenTuiHandle frame,
              std::int32_t x,
              std::int32_t y,
              std::uint32_t width,
              std::uint32_t height) const;

    OpenTuiHandle handle() const { return handle_; }
    bool valid() const { return handle_ != 0; }

private:
    OpenTuiHandle handle_{0};
};

} // namespace arbiter::opentui
