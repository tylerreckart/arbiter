#pragma once

#include "tui/opentui/c_api.h"

#include <array>
#include <cstdint>
#include <functional>

namespace arbiter::opentui {

// Pack an 8-bit-per-channel RGBA for OpenTUI's native color slots (INTENT_RGB).
inline std::array<std::uint16_t, 4> rgba8(std::uint8_t r,
                                            std::uint8_t g,
                                            std::uint8_t b,
                                            std::uint8_t a = 255) {
    return {r, g, b, a};
}

class Engine {
public:
    explicit Engine(std::uint32_t width, std::uint32_t height);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void setup_terminal(bool alternate_screen = true);
    void shutdown_terminal();

    void resize(std::uint32_t width, std::uint32_t height);
    void set_render_offset(std::uint32_t offset);

    using DrawFn = std::function<void(OpenTuiHandle buffer, std::uint32_t width, std::uint32_t height)>;
    void draw(const DrawFn& draw);
    void render(bool force = false);

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] OpenTuiHandle handle() const { return renderer_; }

private:
    OpenTuiHandle renderer_{0};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    bool terminal_ready_{false};
};

} // namespace arbiter::opentui
