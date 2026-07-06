#pragma once

#include "tui/opentui/engine.h"

#include <cstdint>
#include <functional>
#include <mutex>

namespace arbiter::opentui {

// One OpenTUI renderer per REPL session (shared by all panes in Phase 1).
class Session {
public:
    Session() = default;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start(std::uint32_t width, std::uint32_t height);
    void resize(std::uint32_t width, std::uint32_t height);
    void shutdown();

    [[nodiscard]] Engine& engine() { return *engine_; }
    [[nodiscard]] bool active() const { return engine_ != nullptr; }

    OpenTuiHandle begin_frame();
    void end_frame();

    // Run draw_fn while holding the frame lock (begin → draw → render).
    void with_frame(const std::function<void(OpenTuiHandle)>& fn);

    // Re-apply terminal background + cursor from the active TuiDesign.
    void apply_design();
    void flush_display();

    [[nodiscard]] OpenTuiHandle frame() const { return in_frame_ ? frame_ : 0; }

private:
    Engine* engine_{nullptr};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    OpenTuiHandle frame_{0};
    bool in_frame_{false};
    std::mutex frame_mu_;
};

} // namespace arbiter::opentui
