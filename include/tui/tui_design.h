#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace arbiter {

using TuiRgba = std::array<std::uint16_t, 4>;

struct TuiDesign {
    struct Background {
        TuiRgba base;
        TuiRgba panel;
        TuiRgba header;
        TuiRgba scroll;
        TuiRgba status;
        TuiRgba input;
        TuiRgba footer;
        TuiRgba gutter;
    } bg;

    struct Text {
        TuiRgba primary;
        TuiRgba muted;
        TuiRgba subtle;
        TuiRgba inverse;
    } text;

    struct Accent {
        TuiRgba primary;
        TuiRgba secondary;
        TuiRgba success;
        TuiRgba warning;
        TuiRgba error;
        TuiRgba info;
    } accent;

    struct Border {
        TuiRgba subtle;
        TuiRgba focus;
        TuiRgba gutter;
        std::string vertical = "\u2502";
        std::string horizontal = "\u2500";
    } border;

    struct Layout {
        int pane_padding_x = 1;
        int header_padding_x = 1;
        int status_inset_x = 2;
        int input_padding_x = 1;
        int footer_gap = 1;
        int compact_cols = 72;
        int dense_cols = 88;
        bool show_footer = true;
        bool status_pill = true;
    } layout;

    struct Component {
        std::string prompt;
        std::string continuation_prompt = "\u2026 ";
        std::string inactive_prompt;
        std::string agent_prefix = " ";
        std::string agent_suffix = " ";
        std::string status_prefix = " ";
        std::string status_suffix = " ";
        std::string footer_left = "esc interrupt  pgup/dn scroll";
        std::string footer_right = "/agents list agents  /help list commands";
        std::string footer_left_compact = "esc cancel  pg scroll";
        std::string footer_right_compact = "/help";
    } component;
};

TuiRgba tui_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                 std::uint8_t a = 255);

const TuiDesign& tui_design();
void load_tui_design(const std::string& config_dir);

} // namespace arbiter
