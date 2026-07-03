#include "tui/opentui/engine.h"

#include "tui/tui_design.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace arbiter::opentui {

namespace {

void log_bridge(std::uint8_t level, const char* msg, std::uint32_t len) {
    if (level <= 1) return;   // skip debug/trace — noisy during terminal setup
    const char* tag = "info";
    if (level >= 3) tag = "error";
    else if (level == 2) tag = "warn";
    else if (level == 1) tag = "debug";
    std::cerr << "[opentui:" << tag << "] "
              << std::string(msg, msg + len) << '\n';
}

} // namespace

Engine::Engine(std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height) {
    setLogCallback(log_bridge);

    renderer_ = createRenderer(width, height,
                               /*buffered_destination_kind=*/0,
                               /*remote_mode_value=*/1,
                               /*feed_ptr=*/nullptr);
    if (renderer_ == 0) {
        throw std::runtime_error("createRenderer failed");
    }

    setUseThread(renderer_, false);
    setClearOnShutdown(renderer_, true);

    setBackgroundColor(renderer_, arbiter::tui_design().bg.base.data());
}

Engine::~Engine() {
    if (terminal_ready_) {
        restoreTerminalModes(renderer_);
        terminal_ready_ = false;
    }
    if (renderer_ != 0) {
        destroyRenderer(renderer_);
        renderer_ = 0;
    }
}

void Engine::setup_terminal(bool alternate_screen) {
    setupTerminal(renderer_, alternate_screen);
    terminal_ready_ = true;
}

void Engine::shutdown_terminal() {
    if (!terminal_ready_) return;
    restoreTerminalModes(renderer_);
    terminal_ready_ = false;
}

void Engine::resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return;
    width_ = width;
    height_ = height;
    resizeRenderer(renderer_, width, height);
}

void Engine::set_render_offset(std::uint32_t offset) {
    setRenderOffset(renderer_, offset);
}

void Engine::draw(const DrawFn& draw_fn) {
    const OpenTuiHandle buffer = getNextBuffer(renderer_);
    if (buffer == 0) return;
    draw_fn(buffer, getBufferWidth(buffer), getBufferHeight(buffer));
}

void Engine::render(bool force) {
    ::render(renderer_, force);
}

} // namespace arbiter::opentui
