#include "tui/opentui/diff_view.h"

#include "tui/tui_design.h"

namespace arbiter::opentui {

namespace {

void copy_rgba(const TuiRgba& src, std::uint16_t dst[4]) {
    for (int i = 0; i < 4; ++i) dst[i] = src[i];
}

OpenTuiDiffOptions make_default_options(const DiffView::Options& opts) {
    const auto& d = tui_design();
    OpenTuiDiffOptions out{};
    out.view_mode = opts.view_mode;
    out.wrap_mode = opts.wrap_mode;
    out.show_line_numbers = opts.show_line_numbers;
    copy_rgba(tui_rgba(0x1a, 0x4d, 0x1a), out.added_bg);
    copy_rgba(tui_rgba(0x4d, 0x1a, 0x1a), out.removed_bg);
    copy_rgba(d.bg.scroll, out.context_bg);
    copy_rgba(tui_rgba(0x88, 0x88, 0x88), out.line_number_fg);
    copy_rgba(d.accent.success, out.added_sign_color);
    copy_rgba(d.accent.error, out.removed_sign_color);
    return out;
}

} // namespace

DiffView::DiffView() : DiffView(Options{}) {}

DiffView::DiffView(const Options& opts) {
    const OpenTuiDiffOptions native = make_default_options(opts);
    handle_ = createDiffView(&native);
}

DiffView::~DiffView() {
    if (handle_ != 0) destroyDiffView(handle_);
}

bool DiffView::set_patch(std::string_view patch) {
    if (handle_ == 0) return false;
    return diffViewSetPatch(handle_, patch.data(),
                            static_cast<std::uint32_t>(patch.size()));
}

bool DiffView::set_view_mode(std::uint8_t mode) {
    if (handle_ == 0) return false;
    return diffViewSetViewMode(handle_, mode);
}

void DiffView::set_wrap_mode(std::uint8_t mode) {
    if (handle_ == 0) return;
    diffViewSetWrapMode(handle_, mode);
}

void DiffView::set_wrap_width(std::uint32_t content_width) {
    if (handle_ == 0) return;
    diffViewSetWrapWidth(handle_, content_width);
}

void DiffView::set_scroll_y(std::uint32_t offset) {
    if (handle_ == 0) return;
    diffViewSetScrollY(handle_, offset);
}

std::uint32_t DiffView::virtual_line_count() const {
    if (handle_ == 0) return 0;
    return diffViewGetVirtualLineCount(handle_);
}

std::uint32_t DiffView::hunk_count() const {
    if (handle_ == 0) return 0;
    return diffViewGetHunkCount(handle_);
}

std::uint32_t DiffView::hunk_start_line(std::uint32_t hunk_index) const {
    if (handle_ == 0) return UINT32_MAX;
    return diffViewGetHunkStartLine(handle_, hunk_index);
}

void DiffView::draw(OpenTuiHandle frame,
                    std::int32_t x,
                    std::int32_t y,
                    std::uint32_t width,
                    std::uint32_t height) const {
    if (handle_ == 0) return;
    bufferDrawDiffView(frame, handle_, x, y, width, height);
}

} // namespace arbiter::opentui
