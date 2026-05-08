// arbiter/src/theme.cpp — see theme.h
//
// Default theme: OneDark, matched to the canonical palette from
// joshdick/onedark.vim.  Values are hex triplets emitted as 24-bit
// true-color ANSI.  See Theme fields in theme.h for semantic roles.

#include "theme.h"

#include <array>
#include <cstdio>
#include <string>

namespace arbiter {

namespace {

// 24-bit true-color foreground escape.  Kept as a tiny helper so each
// palette entry reads as an RGB triple rather than an opaque escape
// string — keeps the theme table human-scannable.
std::string fg(int r, int g, int b) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[38;2;%d;%d;%dm", r, g, b);
    return buf;
}

Theme build_onedark() {
    Theme t;
    // Attribute-only — same in every theme.
    t.reset     = "\033[0m";
    t.dim       = "\033[2m";
    t.bold      = "\033[1m";
    t.italic    = "\033[3m";
    t.underline = "\033[4m";
    t.strike    = "\033[9m";

    // OneDark palette (https://github.com/joshdick/onedark.vim):
    //   Red         #e06c75   224 108 117
    //   DarkRed     #be5046   190  80  70
    //   Green       #98c379   152 195 121
    //   Yellow      #e5c07b   229 192 123
    //   DarkYellow  #d19a66   209 154 102      (orange)
    //   Blue        #61afef    97 175 239
    //   Purple      #c678dd   198 120 221
    //   Cyan        #56b6c2    86 182 194
    //   White       #abb2bf   171 178 191      (fg)
    //   Comment     #5c6370    92  99 112
    //   NonText     #3e4452    62  68  82
    //   CursorGrey  #2c323c    44  50  60

    // Semantic roles.
    t.accent_focused  = fg( 97, 175, 239);   // Blue       — focused pane border
    t.accent_prompt   = fg(229, 192, 123);   // Yellow     — confirm "[y/N]"
    t.accent_error    = fg(224, 108, 117);   // Red        — errors / denied
    t.accent_success  = fg(152, 195, 121);   // Green      — ✓, accepted
    t.accent_warning  = fg(229, 192, 123);   // Yellow     — warnings (alias of prompt)
    t.accent_info     = fg( 97, 175, 239);   // Blue       — info / titles

    // Chrome.
    t.border_inactive = fg( 62,  68,  82);   // NonText    — pane dividers
    t.border_active   = t.accent_focused;    // alias — Blue
    t.text_dim        = fg( 92,  99, 112);   // Comment    — hint text, status
    t.text_dimmer     = fg( 62,  68,  82);   // NonText    — sub-agent progress
    t.prompt_color    = fg( 92,  99, 112);   // Comment    — "> " prompt
    t.user_echo_arrow = fg( 92,  99, 112);   // Comment    — echo arrow
    t.user_echo_text  = fg(171, 178, 191);   // White      — echoed user text

    // Agent color cycle.  Kept distinct from accent_focused so the focus
    // indicator (blue) doesn't collide with the "index" master identity.
    // Orange stays the canonical "index" accent the way it always has.
    t.agent_master    = fg(209, 154, 102);   // DarkYellow — "index"
    t.agent_palette   = {
        fg(224, 108, 117),   // Red
        fg(229, 192, 123),   // Yellow
        fg(209, 154, 102),   // DarkYellow (orange)
        fg(152, 195, 121),   // Green
        fg( 97, 175, 239),   // Blue
        fg(198, 120, 221),   // Purple
        fg( 86, 182, 194),   // Cyan
        fg(171, 178, 191),   // White
        fg(190,  80,  70),   // DarkRed
        fg(181, 141, 206),   // Lavender (Purple lightened)
        fg( 68, 136, 199),   // Muted Blue (Blue darkened)
        fg(184, 228, 151),   // Light Green
    };

    // Markdown.
    t.md_code     = fg(209, 154, 102);       // DarkYellow — code / inline code
    t.md_link     = fg( 97, 175, 239);       // Blue       — hyperlinks
    t.md_bullet   = fg( 92,  99, 112);       // Comment    — dim list bullets
    t.md_cmd_line = fg(209, 154, 102);       // DarkYellow — agent /cmd passthrough
    t.md_heading  = {
        fg( 97, 175, 239),                   // h1 Blue
        fg(198, 120, 221),                   // h2 Purple
        fg( 86, 182, 194),                   // h3 Cyan
        fg(209, 154, 102),                   // h4+ DarkYellow
    };
    return t;
}

} // namespace

const Theme& theme() {
    static const Theme kOneDark = build_onedark();
    return kOneDark;
}

} // namespace arbiter
