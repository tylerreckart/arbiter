#include "tui/opentui/top_header_frame.h"

#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace arbiter::opentui {

namespace {

constexpr std::uint32_t kAttrBold = 1u << 0;
constexpr int kEdgePad = 1;

int cell_width(std::string_view s) {
    int w = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
}

std::string trim_to_cells(std::string s, int max_cells) {
    if (max_cells <= 0) return {};
    while (!s.empty() && cell_width(s) > max_cells) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
    }
    return s;
}

void fill_rect(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::uint32_t w,
               std::uint32_t h,
               const TuiRgba& bg) {
    if (w == 0 || h == 0) return;
    bufferFillRect(frame, x, y, w, h, bg.data());
}

void draw_text(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::string_view text,
               const TuiRgba& fg,
               const TuiRgba& bg,
               std::uint32_t attrs = 0) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg.data(),
                   attrs);
}

std::string version_string() {
#ifdef INDEX_VERSION
    return std::string("v") + INDEX_VERSION;
#else
    return "vdev";
#endif
}

} // namespace

void draw_top_header(OpenTuiHandle frame, int cols) {
    if (frame == 0 || cols <= 0) return;

    const TuiDesign& d = tui_design();
    const TuiRgba& bg = d.bg.header;
    const TuiRgba& accent = d.accent.primary;

    fill_rect(frame, 0, 0, static_cast<std::uint32_t>(cols), 1, bg);

    const std::string version = version_string();
    const int version_cells = cell_width(version);
    const int version_x = std::max(kEdgePad, cols - kEdgePad - version_cells);

    const int name_x = kEdgePad + 2; // symbol cell + one space gap
    const int name_max = std::max(0, version_x - 1 - name_x);
    const std::string name = trim_to_cells("Arbiter", name_max);

    draw_text(frame, static_cast<std::uint32_t>(kEdgePad), 0, "⛮", accent, bg, kAttrBold);
    draw_text(frame, static_cast<std::uint32_t>(name_x), 0, name, d.text.primary, bg);

    if (version_x > name_x + cell_width(name)) {
        draw_text(frame, static_cast<std::uint32_t>(version_x), 0, version, d.text.subtle, bg);
    }
}

} // namespace arbiter::opentui
