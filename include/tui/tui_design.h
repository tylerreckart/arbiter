#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
        bool show_history_sidebar = true;
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

    // Markdown / scrollback content colors (OneDark defaults; overridable via
    // the "content" group in tui.json).
    struct Content {
        std::array<TuiRgba, 4> heading{};
        TuiRgba code{};
        TuiRgba link{};
        TuiRgba bullet{};
        TuiRgba blockquote{};
        TuiRgba rule{};
        TuiRgba writ_line{};
        TuiRgba diff_add{};
        TuiRgba diff_remove{};
        TuiRgba diff_hunk{};
        TuiRgba diff_file{};
        TuiRgba success{};
        TuiRgba error{};
        TuiRgba warning{};
        TuiRgba info{};
        TuiRgba text_dim{};
        TuiRgba text_dimmer{};
        TuiRgba accent_focused{};
        TuiRgba accent_prompt{};
        TuiRgba prompt_color{};
        TuiRgba user_echo_arrow{};
        TuiRgba user_echo_text{};
        TuiRgba border_inactive{};
        TuiRgba agent_master{};
        std::array<TuiRgba, 12> agent_palette{};
    } content;
};

// Readable sidebar text on `bg.header` (presets often set text.subtle too close).
struct SidebarColors {
    TuiRgba label{}; // KV keys, empty states, de-emphasized marks
    TuiRgba value{}; // metrics, tool names, secondary lines
    TuiRgba body{};  // agent id, tasks, todo subjects
};

TuiRgba tui_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                 std::uint8_t a = 255);

[[nodiscard]] SidebarColors tui_sidebar_colors(const TuiDesign& d);

const TuiDesign& tui_design();
// `cli_preset` overrides `"preset"` in tui.json for this session; other
// tui.json tokens still apply on top.
void load_tui_design(const std::string& config_dir,
                     std::string_view cli_preset = {});
// Bumps when load_tui_design() completes — theme() uses this to invalidate cache.
[[nodiscard]] std::uint32_t tui_design_generation();

// Built-in full themes (chrome + scrollback content).  Set `"preset"` in
// ~/.arbiter/tui.json — see docs/tui/index.md.
inline constexpr const char* kDefaultTuiPreset = "onedark";

[[nodiscard]] std::vector<std::string> tui_builtin_presets();
[[nodiscard]] bool tui_preset_is_valid(std::string_view name);
[[nodiscard]] TuiDesign tui_design_for_preset(std::string_view preset);
void set_show_history_sidebar(const std::string& config_dir, bool show);

} // namespace arbiter
