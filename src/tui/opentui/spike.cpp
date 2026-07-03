#include "tui/opentui/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace arbiter {

namespace {

std::atomic<bool> g_resize_pending{false};
std::atomic<bool> g_interrupt{false};

void on_sigwinch(int) {
    g_resize_pending.store(true, std::memory_order_relaxed);
}

void on_sigint(int) {
    g_interrupt.store(true, std::memory_order_relaxed);
}

struct TermSize {
    std::uint32_t cols;
    std::uint32_t rows;
};

TermSize query_terminal_size() {
    TermSize size{80, 24};
#if defined(TIOCGWINSZ)
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        size.cols = static_cast<std::uint32_t>(ws.ws_col);
        size.rows = static_cast<std::uint32_t>(ws.ws_row);
    }
#endif
    return size;
}

void draw_spike_frame(OpenTuiHandle buffer, std::uint32_t width, std::uint32_t height) {
    using arbiter::opentui::rgba8;

    const auto bg = rgba8(0x1e, 0x1e, 0x2e);
    const auto border = rgba8(0x89, 0xb4, 0xfa);
    const auto fg = rgba8(0xcd, 0xd6, 0xf4);
    const auto dim = rgba8(0x6c, 0x70, 0x86);

    bufferFillRect(buffer, 0, 0, width, height, bg.data());

    if (width < 4 || height < 4) return;

    const std::uint32_t x0 = 1;
    const std::uint32_t y0 = 1;
    const std::uint32_t box_w = width - 2;
    const std::uint32_t box_h = height - 2;

    bufferFillRect(buffer, x0, y0, box_w, box_h, rgba8(0x18, 0x18, 0x25).data());

    auto hline = [&](std::uint32_t y) {
        const std::string line(box_w, '-');
        bufferDrawText(buffer, line.data(), static_cast<std::uint32_t>(line.size()),
                       x0, y, border.data(), nullptr, 0);
    };

    hline(y0);
    if (box_h > 1) hline(y0 + box_h - 1);

    for (std::uint32_t y = y0 + 1; y + 1 < y0 + box_h; ++y) {
        bufferDrawText(buffer, "|", 1, x0, y, border.data(), nullptr, 0);
        if (box_w > 1) {
            bufferDrawText(buffer, "|", 1, x0 + box_w - 1, y, border.data(), nullptr, 0);
        }
    }

    const std::string title = "arbiter OpenTUI spike";
    if (box_w > title.size() + 2) {
        const std::uint32_t tx = x0 + (box_w - static_cast<std::uint32_t>(title.size())) / 2;
        bufferDrawText(buffer, title.data(), static_cast<std::uint32_t>(title.size()),
                       tx, y0 + 1, fg.data(), nullptr, 0);
    }

    const std::string hint = "Phase 0 — press q to quit";
    if (box_h > 4 && box_w > hint.size() + 2) {
        const std::uint32_t hx = x0 + (box_w - static_cast<std::uint32_t>(hint.size())) / 2;
        bufferDrawText(buffer, hint.data(), static_cast<std::uint32_t>(hint.size()),
                       hx, y0 + 3, dim.data(), nullptr, 0);
    }

    const std::string dims = std::to_string(width) + "x" + std::to_string(height);
    if (box_h > 6 && box_w > dims.size() + 2) {
        const std::uint32_t dx = x0 + (box_w - static_cast<std::uint32_t>(dims.size())) / 2;
        bufferDrawText(buffer, dims.data(), static_cast<std::uint32_t>(dims.size()),
                       dx, y0 + 5, dim.data(), nullptr, 0);
    }
}

bool poll_quit_key() {
    unsigned char ch{};
    const ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0) return false;
    return ch == 'q' || ch == 'Q' || ch == 3; // Ctrl+C in raw mode sometimes arrives as ETX
}

} // namespace

void cmd_tui_spike() {
    std::signal(SIGINT, on_sigint);
#if defined(SIGWINCH)
    std::signal(SIGWINCH, on_sigwinch);
#endif

    auto size = query_terminal_size();
    opentui::Engine engine(size.cols, size.rows);
    engine.setup_terminal(true);

    bool running = true;
    while (running) {
        if (g_interrupt.load(std::memory_order_relaxed)) {
            running = false;
            break;
        }

        if (g_resize_pending.exchange(false, std::memory_order_relaxed)) {
            size = query_terminal_size();
            engine.resize(size.cols, size.rows);
        }

        engine.draw(draw_spike_frame);
        engine.render(false);

        if (poll_quit_key()) {
            running = false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    engine.shutdown_terminal();
}

} // namespace arbiter
