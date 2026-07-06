// arbiter/src/theme.cpp — see theme.h

#include "theme.h"
#include "tui/tui_design.h"

#include <cstdio>
#include <string>

namespace arbiter {

namespace {

std::string fg_rgba(const TuiRgba& c) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[38;2;%u;%u;%um",
                  static_cast<unsigned>(c[0]),
                  static_cast<unsigned>(c[1]),
                  static_cast<unsigned>(c[2]));
    return buf;
}

Theme build_theme_from_design() {
    const TuiDesign& d = tui_design();
    const auto& c = d.content;

    Theme t;
    t.reset     = "\033[0m";
    t.dim       = "\033[2m";
    t.bold      = "\033[1m";
    t.italic    = "\033[3m";
    t.underline = "\033[4m";
    t.strike    = "\033[9m";

    t.accent_focused  = fg_rgba(c.accent_focused);
    t.accent_prompt   = fg_rgba(c.accent_prompt);
    t.accent_error    = fg_rgba(c.error);
    t.accent_success  = fg_rgba(c.success);
    t.accent_warning  = fg_rgba(c.warning);
    t.accent_info     = fg_rgba(c.info);

    t.border_inactive = fg_rgba(c.border_inactive);
    t.border_active   = t.accent_focused;
    t.text_dim        = fg_rgba(c.text_dim);
    t.text_dimmer     = fg_rgba(c.text_dimmer);
    t.prompt_color    = fg_rgba(c.prompt_color);
    t.user_echo_arrow = fg_rgba(c.user_echo_arrow);
    t.user_echo_text  = fg_rgba(c.user_echo_text);

    t.agent_master = fg_rgba(c.agent_master);
    for (size_t i = 0; i < t.agent_palette.size(); ++i) {
        t.agent_palette[i] = fg_rgba(c.agent_palette[i]);
    }

    t.md_code     = fg_rgba(c.code);
    t.md_code_keyword  = fg_rgba(c.code_keyword);
    t.md_code_string   = fg_rgba(c.code_string);
    t.md_code_comment  = fg_rgba(c.code_comment);
    t.md_code_number   = fg_rgba(c.code_number);
    t.md_code_type     = fg_rgba(c.code_type);
    t.md_code_function = fg_rgba(c.code_function);
    t.md_link     = fg_rgba(c.link);
    t.md_bullet   = fg_rgba(c.bullet);
    t.md_cmd_line = fg_rgba(c.writ_line);
    for (size_t i = 0; i < t.md_heading.size(); ++i) {
        t.md_heading[i] = fg_rgba(c.heading[i]);
    }
    return t;
}

} // namespace

const Theme& theme() {
    static Theme cached = build_theme_from_design();
    static std::uint32_t generation = tui_design_generation();
    const std::uint32_t current = tui_design_generation();
    if (generation != current) {
        cached = build_theme_from_design();
        generation = current;
    }
    return cached;
}

} // namespace arbiter
