#pragma once
// Decodes SGR extended mouse reports: CSI < Pb ; Px ; Py M/m
//
// OpenTUI enables these via enableMouse() (DECSET 1000/1002 + 1006). Arbiter
// owns stdin, so we parse the wire format ourselves rather than relying on
// OpenTUI's TypeScript MouseParser. Coordinates are converted to 0-based
// cells to match LayoutTree / Rect / bufferFillRect.

#include <cstdint>
#include <optional>
#include <string_view>

namespace arbiter::opentui {

enum class MouseType {
    Down,
    Up,
    Drag,
    Move,
    Scroll,
};

enum class MouseButton {
    Left = 0,
    Middle = 1,
    Right = 2,
    None = 3,
};

enum class ScrollDir {
    Up,
    Down,
    Left,
    Right,
};

struct MouseEvent {
    MouseType   type = MouseType::Down;
    MouseButton button = MouseButton::Left;
    int         x = 0;   // 0-based column
    int         y = 0;   // 0-based row
    bool        shift = false;
    bool        alt = false;
    bool        ctrl = false;
    ScrollDir   scroll = ScrollDir::Up;
    int         scroll_delta = 0;
};

// `params` is the CSI parameter string including the leading '<', e.g.
// "<0;10;5" for a left-button press at (10,5) 1-based. `final` is 'M'
// (press / scroll / drag) or 'm' (release).
inline std::optional<MouseEvent> decode_sgr_mouse(std::string_view params,
                                                  char final) {
    if (params.empty() || params[0] != '<') return std::nullopt;
    if (final != 'M' && final != 'm') return std::nullopt;

    int values[3] = {0, 0, 0};
    int part = 0;
    bool has_digit = false;
    for (size_t i = 1; i < params.size(); ++i) {
        const char c = params[i];
        if (c >= '0' && c <= '9') {
            has_digit = true;
            values[part] = values[part] * 10 + (c - '0');
            continue;
        }
        if (c == ';') {
            if (!has_digit || part >= 2) return std::nullopt;
            ++part;
            has_digit = false;
            continue;
        }
        return std::nullopt;
    }
    if (!has_digit || part != 2) return std::nullopt;

    const int raw = values[0];
    const int wire_x = values[1];
    const int wire_y = values[2];
    if (wire_x < 1 || wire_y < 1) return std::nullopt;

    MouseEvent ev;
    ev.x = wire_x - 1;
    ev.y = wire_y - 1;
    ev.shift = (raw & 4) != 0;
    ev.alt = (raw & 8) != 0;
    ev.ctrl = (raw & 16) != 0;

    const int button_bits = raw & 3;
    const bool is_scroll = (raw & 64) != 0;
    const bool is_motion = (raw & 32) != 0;

    if (is_scroll) {
        if (final != 'M') return std::nullopt;
        ev.type = MouseType::Scroll;
        ev.button = MouseButton::None;
        ev.scroll_delta = 1;
        switch (button_bits) {
            case 0: ev.scroll = ScrollDir::Up; break;
            case 1: ev.scroll = ScrollDir::Down; break;
            case 2: ev.scroll = ScrollDir::Left; break;
            default: ev.scroll = ScrollDir::Right; break;
        }
        return ev;
    }

    if (is_motion) {
        // With button-event tracking (?1002), motion while a button is held
        // arrives with the button encoded in the low bits (not 3). Bare
        // pointer motion (?1003) uses button==3; we treat that as Move and
        // ignore it in the router unless a future hover feature needs it.
        if (button_bits == 3) {
            ev.type = MouseType::Move;
            ev.button = MouseButton::None;
        } else {
            ev.type = MouseType::Drag;
            ev.button = static_cast<MouseButton>(button_bits);
        }
        return ev;
    }

    if (final == 'M') {
        ev.type = MouseType::Down;
        ev.button = (button_bits == 3) ? MouseButton::None
                                       : static_cast<MouseButton>(button_bits);
    } else {
        ev.type = MouseType::Up;
        ev.button = (button_bits == 3) ? MouseButton::None
                                       : static_cast<MouseButton>(button_bits);
    }
    return ev;
}

}  // namespace arbiter::opentui
