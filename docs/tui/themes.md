# TUI theme JSON

Themes are JSON documents — no colors are hard-coded in the binary. Built-in themes ship as `themes/*.json` (installed to `share/arbiter/themes/`; copied to `~/.arbiter/themes/` on `arbiter --init`). Your config file is `~/.arbiter/tui.json`; custom theme documents live in `~/.arbiter/themes/`.

## Quick start

Pick a built-in preset:

```json
{
  "preset": "nord"
}
```

Or point at a theme file:

```json
{
  "theme_file": "themes/mine.json"
}
```

Paths in `theme_file` are relative to `~/.arbiter/` unless absolute (or `~/…`).

## Create a custom theme

1. Export a built-in preset as a starting point:

```bash
arbiter --export-theme onedark > ~/.arbiter/themes/mine.json
```

2. Edit colors (`#RRGGBB` hex). For a variant of an existing theme, add `"preset": "nord"` and only the keys you want to change; for a standalone theme, export a full file and edit it directly.

3. Activate it:

```json
{
  "theme_file": "themes/mine.json"
}
```

Or name the file `mine.json` and use `/theme mine` / `"preset": "mine"` (user themes in `~/.arbiter/themes/`).

`arbiter --init` copies all built-in theme JSON files into `~/.arbiter/themes/` (skipping files you already edited) and writes `example.json` as an editable copy of the default.

## In-session commands

| Command | Effect |
|---------|--------|
| `/theme list` | Built-in presets + custom themes in `~/.arbiter/themes/` |
| `/theme <name>` | Switch preset or custom theme; updates `tui.json` |
| `/theme save <name>` | Write the current look to `~/.arbiter/themes/<name>.json` |
| `/theme file <path>` | Load a JSON file and set `theme_file` in `tui.json` |

## JSON schema

Top-level keys:

| Key | Purpose |
|-----|---------|
| `preset` | Built-in preset name, or basename of `themes/<name>.json` |
| `theme_file` | Path to a theme JSON file (relative to `~/.arbiter/`) |
| `bg` | Pane backgrounds: `base`, `panel`, `header`, `scroll`, `status`, `input`, `footer`, `gutter` |
| `text` | `primary`, `muted`, `subtle`, `inverse` |
| `accent` | `primary`, `secondary`, `success`, `warning`, `error`, `info` |
| `border` | `subtle`, `focus`, `gutter`, plus optional `vertical` / `horizontal` strings |
| `content` | Markdown/scrollback: `heading` (array of 4), `code`, `link`, diff colors, syntax tokens, `agent_palette` (array of 12), … |
| `layout` | Padding, column breakpoints, `show_footer`, `show_history_sidebar`, … |
| `component` | Prompt/footer strings |

Example partial override on a preset (in `tui.json` or a theme file):

```json
{
  "preset": "onedark",
  "accent": { "primary": "#c678dd" },
  "content": {
    "heading": ["#61afef", "#c678dd", "#56b6c2", "#d19a66"]
  },
  "layout": {
    "pane_padding_x": 1,
    "input_padding_x": 2
  }
}
```

All color values use `"#RRGGBB"`. Settings in `tui.json` are merged on top of `theme_file` / `preset` (useful for small tweaks without duplicating a full theme).
