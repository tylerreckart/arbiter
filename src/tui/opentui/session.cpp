#include "tui/opentui/session.h"
#include "tui/opentui/c_api.h"

#include "cli_helpers.h"
#include "tui/tui_design.h"

namespace arbiter::opentui {

Session::~Session() {
    shutdown();
}

void Session::start(std::uint32_t width, std::uint32_t height) {
    if (engine_) return;
    width_ = width;
    height_ = height;
    engine_ = new Engine(width, height);
    engine_->setup_terminal(true);
    engine_->set_render_offset(0);
    OpenTuiCursorStyleOptions cursor{};
    cursor.style = 0; // block
    cursor.blinking = true;
    cursor.color = tui_design().accent.primary.data();
    cursor.cursor = 0;
    setCursorStyleOptions(engine_->handle(), &cursor);
}

void Session::resize(std::uint32_t width, std::uint32_t height) {
    if (!engine_) return;
    width_ = width;
    height_ = height;
    engine_->resize(width, height);
    engine_->set_render_offset(0);
}

OpenTuiHandle Session::begin_frame() {
    if (!engine_) return 0;
    const std::uint32_t w = static_cast<std::uint32_t>(term_cols());
    const std::uint32_t h = static_cast<std::uint32_t>(term_rows());
    if (w != width_ || h != height_) resize(w, h);

    frame_ = getNextBuffer(engine_->handle());
    in_frame_ = frame_ != 0;
    return frame_;
}

void Session::end_frame() {
    if (!engine_ || !in_frame_) return;
    engine_->render(false);
    in_frame_ = false;
    frame_ = 0;
}

void Session::shutdown() {
    if (!engine_) return;
    engine_->shutdown_terminal();
    delete engine_;
    engine_ = nullptr;
}

} // namespace arbiter::opentui
