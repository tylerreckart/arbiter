#include "tui/opentui/engine.h"

#include "tui/tui_design.h"

#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <unistd.h>

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

// OpenTUI's setupTerminal emits capability queries (OSC 10/11, DECRQM, DCS
// identity, CPR, …).  The terminal replies on stdin.  If we don't consume
// those bytes and feed them to processCapabilityResponse, they sit in the
// kernel buffer and get echoed onto the shell buffer when the TUI exits.
void sync_terminal_capability_responses(OpenTuiHandle renderer) {
    if (renderer == 0) return;

    const int old_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);

    auto set_nonblock = [&](bool on) {
        if (old_flags < 0) return;
        ::fcntl(STDIN_FILENO, F_SETFL,
                on ? (old_flags | O_NONBLOCK) : old_flags);
    };

    auto read_available = [&]() -> std::string {
        std::string out;
        char buf[4096];
        while (true) {
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    };

    // processCapabilityResponse can trigger follow-up queries; a few short
    // rounds drains the typical ghostty/kitty startup burst.
    for (int round = 0; round < 4; ++round) {
        std::string acc;
        const int wait_ms = (round == 0) ? 700 : 120;
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(wait_ms);

        set_nonblock(true);
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            timeval tv = {0, 25000};
            const int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
            if (r > 0) acc += read_available();
            else if (!acc.empty()) break;
        }
        acc += read_available();
        set_nonblock(false);

        if (acc.empty()) break;
        processCapabilityResponse(renderer, acc.data(),
                                  static_cast<std::uint32_t>(acc.size()));
    }
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
    if (renderer_ != 0) {
        // destroyRenderer runs performShutdownSequence (alt-screen exit,
        // cursor restore).  Do not call restoreTerminalModes here — that
        // helper re-enables mouse/focus modes for focus-in events, not exit.
        destroyRenderer(renderer_);
        renderer_ = 0;
    }
    terminal_ready_ = false;
}

void Engine::setup_terminal(bool alternate_screen) {
    setupTerminal(renderer_, alternate_screen);
    sync_terminal_capability_responses(renderer_);
    terminal_ready_ = true;
}

void Engine::shutdown_terminal() {
    if (!terminal_ready_) return;
    terminal_ready_ = false;
    // Terminal cleanup happens in destroyRenderer when Session tears down.
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

void Engine::flush_display() {
    render(true);
}

} // namespace arbiter::opentui
