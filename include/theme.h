#pragma once
// arbiter/include/theme.h
//
// Global color / style palette.  Every ANSI SGR sequence painted anywhere
// in the app should come from a named field on the active Theme, so
// swapping to a different color scheme is a one-line change instead of a
// codebase-wide grep.
//
// The default scheme is OneDark — Joshua Dickens's terminal port of the
// Atom editor theme (https://github.com/joshdick/onedark.vim), adapted to
// the semantic roles this TUI needs (pane borders, focus accent, error /
// success tints, agent color cycle).
//
// All values are 24-bit true-color escapes (\033[38;2;R;G;Bm).  Modern
// terminals (macOS Terminal 10.15+, iTerm2, Windows Terminal, WezTerm,
// Alacritty, Kitty, Ghostty, VS Code, Linux gnome-terminal / konsole /
// foot) render them directly; the one-off fallback for 256-color-only
// terminals would be to map each field to the nearest 256-color code,
// but we don't see that often enough to maintain two palettes.

#include <array>
#include <string>

namespace arbiter {

struct Theme {
    // Attribute-only escapes — color-agnostic, shared across themes.
    std::string reset;       // \033[0m      — clear all attributes
    std::string dim;         // \033[2m      — faint (dims whatever color is active)
    std::string bold;        // \033[1m      — bold weight
    std::string italic;      // \033[3m
    std::string underline;   // \033[4m
    std::string strike;      // \033[9m

    // Semantic foreground colors.
    std::string accent_focused;    // focused pane's header bottom border; master agent
    std::string accent_prompt;     // confirmation dialogs ("[y/N]", pane close)
    std::string accent_error;      // ERR:, [user denied input], [interrupted], ✗ tool fail
    std::string accent_success;    // ✓ tool ok, [user accepted input], [closing]
    std::string accent_warning;    // non-fatal warnings
    std::string accent_info;       // blue info tint

    // Structural chrome.
    std::string border_inactive;   // pane separators, header sep on unfocused panes,
                                   // mid-sep above input, hint separator
    std::string border_active;     // alias for accent_focused — readability at call sites
    std::string text_dim;          // hint text faint parts, status
    std::string text_dimmer;       // sub-agent progress, dimmer than text_dim
    std::string prompt_color;      // the "> " readline prompt arrow
    std::string user_echo_arrow;   // "> " prefix on echoed user input
    std::string user_echo_text;    // echoed user-typed text

    // Stable per-agent color.  "index" master always gets `agent_master`;
    // other agents hash their id into `agent_palette` to pick a color.
    std::string                    agent_master;
    std::array<std::string, 12>    agent_palette;

    // Markdown rendering slots.  MarkdownRenderer paints semantic roles
    // (headings, inline code, links, list bullets, agent command lines)
    // with these; swap in a different theme and markdown recolors with it.
    std::string                    md_code;        // `code` + ```code blocks```
    std::string                    md_link;        // [link]() — underline + this color
    std::string                    md_bullet;      // list bullets (• ◦ –)
    std::string                    md_cmd_line;    // /fetch, /exec, … display-color
    std::array<std::string, 4>     md_heading;     // h1..h4 (h5/h6 reuse h4)
};

// The active theme.  Currently returns OneDark.  Later: read a config
// setting or env var and return whichever theme the user picked.
const Theme& theme();

} // namespace arbiter
