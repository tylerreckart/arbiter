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
        int block_gap = 1;            // blank rows between messages
        int panel_gap = 1;            // blank rows before/after code & diff
        int prose_paragraph_gap = 1;  // max consecutive blank prose lines
        int scroll_pad_y = 0;         // draw-time top/bottom inset in scroll
        int scroll_gutter_cols = 0;   // left gutter cols in scroll (0 = off)
        int compact_cols = 72;
        int dense_cols = 88;
        bool show_footer = true;
        bool status_pill = true;
        bool show_history_sidebar = true;
        // When true, reclaim the blank hint/pad rows in multi-pane (or when
        // show_footer is false) so scroll gets the vertical space back.
        bool chrome_compact_rows = true;
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
        std::string footer_left_compact = "pg scroll";
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
        TuiRgba code_keyword{};
        TuiRgba code_string{};
        TuiRgba code_comment{};
        TuiRgba code_number{};
        TuiRgba code_type{};
        TuiRgba code_function{};
        TuiRgba code_bg{};
        TuiRgba code_header_bg{};
        TuiRgba code_gutter{};
        TuiRgba diff_bg_context{};
        TuiRgba diff_bg_add{};
        TuiRgba diff_bg_remove{};
        TuiRgba diff_bg_empty{};
        TuiRgba system_fg{};
        TuiRgba text_dim{};
        TuiRgba text_dimmer{};
        TuiRgba accent_focused{};
        TuiRgba accent_prompt{};
        TuiRgba prompt_color{};
        TuiRgba user_echo_arrow{};
        TuiRgba user_echo_text{};
        // Scrollback strip behind echoed user turns — defaults to bg.header
        // (same surface as the live readline input block).
        TuiRgba user_echo_bg{};
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

// Soft horizontal content pad: 0 at/below compact_cols, full pane_padding_x
// at/above dense_cols, lerped in between.
[[nodiscard]] int tui_pane_pad_x(int cols, const TuiDesign& d);

// Clamped pad used by pane chrome and sidebars (never more than (cols-1)/2).
[[nodiscard]] int tui_pane_edge_pad(int cols, const TuiDesign& d);

// Soft input inner pad — follows the same compact/dense ramp as pane pad.
[[nodiscard]] int tui_input_pad_x(int cols, const TuiDesign& d);

// Rows reserved below the input block.  Full budget (3) when the footer
// hint is shown; shrinks to 1 when chrome_compact_rows reclaims space.
[[nodiscard]] int tui_bottom_pad_rows(bool footer_hint_visible, const TuiDesign& d);

[[nodiscard]] SidebarColors tui_sidebar_colors(const TuiDesign& d);

// Sidebar panel background — deliberately darker than `bg.base` so both
// sidebars read as recessed relative to the main pane, regardless of theme.
[[nodiscard]] TuiRgba tui_sidebar_bg(const TuiDesign& d);

const TuiDesign& tui_design();
// `cli_preset` overrides `"preset"` in tui.json for this session; other
// tui.json tokens still apply on top.
void load_tui_design(const std::string& config_dir,
                     std::string_view cli_preset = {});
// Bumps when load_tui_design() completes — theme() uses this to invalidate cache.
[[nodiscard]] std::uint32_t tui_design_generation();

// Built-in themes ship as JSON in share/arbiter/themes (or ARBITER_THEMES_DIR at
// build time). Set `"preset"` or `"theme_file"` in ~/.arbiter/tui.json.
inline constexpr const char* kDefaultTuiPreset = "onedark";

[[nodiscard]] std::string tui_bundled_themes_dir();
[[nodiscard]] std::vector<std::string> tui_builtin_presets();
[[nodiscard]] std::vector<std::string> tui_user_theme_names(const std::string& config_dir);
[[nodiscard]] std::vector<std::string> tui_list_available_themes(const std::string& config_dir);
[[nodiscard]] std::string tui_themes_dir(const std::string& config_dir);
[[nodiscard]] bool tui_preset_is_valid(std::string_view name);
[[nodiscard]] bool tui_theme_name_is_valid(const std::string& config_dir, std::string_view name);
[[nodiscard]] TuiDesign tui_design_for_preset(std::string_view preset);
[[nodiscard]] std::string tui_active_preset();
[[nodiscard]] std::string tui_active_theme_file();
[[nodiscard]] std::string tui_design_to_json(const TuiDesign& design,
                                             std::string_view preset = {});
[[nodiscard]] bool tui_write_theme_file(const std::string& path,
                                        const TuiDesign& design,
                                        std::string_view preset = {});
void tui_install_bundled_themes(const std::string& config_dir, bool force = false);
void set_tui_preset(const std::string& config_dir, std::string_view preset);
bool set_tui_theme_file(const std::string& config_dir, std::string_view theme_file);
void set_show_history_sidebar(const std::string& config_dir, bool show);

} // namespace arbiter
