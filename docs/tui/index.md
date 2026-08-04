# Arbiter TUI

Arbiter's interactive terminal interface. One process, one shell window — but inside that window: a multi-pane layout where each pane is an independent conversation with its own agent, history, and streaming output. The TUI is what you get when you run `arbiter` with no arguments.

Rendering uses [OpenTUI](https://github.com/anomalyco/opentui) (native cell-buffer diff renderer). Per-pane chrome, scrollback, and input are composited each frame by the output pump (~30 ms).

A pane is to a conversation what a tab is to a browser. A new pane is a new conversation against an agent of your choosing; multiple panes run side-by-side or stacked, each independently typing, streaming, and waiting for its agent. Background loops (long-running agent processes) live alongside; foreground panes can spawn child panes (`/pane <agent> <msg>`) whose results land back in the spawner when done.

Start with `arbiter`. The default layout is a single pane covering the main terminal area. A **left-hand conversation sidebar** lists prior threads and lets you start a new one (`Ctrl-w b` to enter, `Ctrl-w t` to toggle visibility). A **right-hand sidebar** shows session usage, agent/model, todos, and recent tool activity when the terminal is wide enough (MCP appears only after MCP tools are used); `Ctrl-w s` toggles it. `Ctrl-w v` / `Ctrl-w h` split the main area.

## Screen anatomy

Every pane has the same chrome layout. From top to bottom:

```
┌──────────────────────────────────────────────────────────┐
│ agent · session title                       status/stats │  row 1: identity + status
│ ───────────────────────────────────────────────────────  │  row 2: header separator
│                                                          │
│   streamed model output, tool-call summaries, /cmd       │  scroll region
│   results, system messages …                             │  (scrolls; bounded ring)
│                                                          │
│ ─── [⠋ 3 tool calls…] ──────────────────────────────────  │  mid separator
│ ❯ user input here, multi-line if it wraps                │  input area (1..5 rows)
│ ─────────────────────────────────────────────────────── │  hint separator
│ esc interrupt · pgup/dn scroll · /agents · /help         │  hint row
└──────────────────────────────────────────────────────────┘
```

What lives where:

- **Identity row.** Left side: focused agent name and the session title. Right side: live status when the agent is working ("thinking…"), or aggregate token / cost stats when idle.
- **Header separator.** Plain dashed line; gets an accent colour on the focused pane in multi-pane layouts.
- **Scroll region.** Where everything the agent emits goes — streamed prose, tool-call summary lines, `/cmd` output, system notices. Scrollable with PgUp/PgDn (virtual-line aware: wrapped lines count as multiple rows). Markdown/theme ANSI is rendered as OpenTUI syntax highlights.
- **Mid separator.** Dashed line above the input. Doubles as the tool-call indicator: while a turn is firing tool calls, this row shows `⠋ N tool calls…` instead of plain dashes.
- **Input area.** 1 row by default, grows up to 5 as text wraps. Standard editing controls (arrows, history, tab-complete on slash commands). Rendered by OpenTUI `EditBuffer` / `EditorView`.
- **Hint row.** Static legend of the most-used keys and commands. In multi-pane layouts the compact chord hint (`^W w/z/c`) paints on the focused column's outer-bottom pane. Outer-bottom panes keep a shared footer pad so column bottoms stay aligned; stacked panes above that edge use a single-cell separator gutter (no trailing pad; output float only on outer-top). `chrome_compact_rows` reclaims pad only when `show_footer` is off.

### Sidebar (wide terminals)

When the terminal is at least 96 columns wide **and only one pane is open**, a fixed right column (~24–28 cols) shows live session telemetry after the first prompt is submitted (restored sessions with history show it immediately). The sidebar hides automatically below 96 columns, when a second pane is split open, or when toggled off with `Ctrl-w s`.

- **Context** — context window fill (`used` % and token fraction from the last turn; session `peak` %), plus cumulative in/out tokens, cost, and turn count.
- **Agent** — focused pane's current agent and model; last turn model when different.
- **Task** — focused pane's pinned original task (advisor-gated work).
- **Todos** — open `/todo` items tracked this session (when present).
- **Scheduled** — `/schedule` entries and active `/loop` background tasks (when present).
- **Tools** — recent tool calls with descriptive labels (`exec: git status`, `write: path`, …) and ✓/✗ status; live count while a turn runs tools.
- **MCP** — recent MCP invocations as `server.tool`, listed separately.

Token totals also appear in the pane header stats row (right side of row 1) when idle. `/tokens` prints the full breakdown in scrollback.

## Where to next

- **[Slash commands](commands.md)** — the full `/cmd` catalogue, grouped by category.
- **[Keybindings](keybindings.md)** — every key, chord, and modifier the editor recognizes.
- **[Panes](panes.md)** — multi-pane layouts: split, focus, close, `/pane` spawn semantics.
- **[Streaming](streaming.md)** — what you see during a turn: thinking spinner, tool-call indicator, verbose mode, cancellation.
- **[Sessions](sessions.md)** — global conversations, autosave / mid-turn checkpoints, compaction.
- **[Output UX](output-ux.md)** — tool timeline, thinking rows, permission cards, replay chrome.
- **[Themes](themes.md)** — `TuiDesign` schema, theme tokens, and spacing.

## Configuration

Per-user state lives under `~/.arbiter/`:

| Path                       | What it is                                              |
|----------------------------|---------------------------------------------------------|
| `openrouter_api_key`       | OpenRouter API key for hosted models.                   |
| `agents/*.json`            | Agent constitutions — one file per agent.               |
| `conversations/`           | Legacy TUI JSON archive + `layout.json` mirror (sessions live in `tenants.db`). |
| `tenants.db`               | Tenant store: conversations (TUI + API), scratchpads, structured memory, schedules, todos, lessons (TUI and `--api`). |
| `sessions/*.json`          | Legacy per-cwd snapshots (imported once into the conversation store). |
| `memory/t<tid>/`           | Legacy filesystem scratchpad fallback (DB is primary).  |

Pane layout restores from `tui_prefs` / `conversations/layout.json` on relaunch (split tree + per-pane conversation bindings). Painted scrollback is rebuilt via transcript replay, not a pixel buffer; see [Sessions](sessions.md).

## TUI design config

Themes are JSON-driven. See **[themes.md](themes.md)** for the full schema, export workflow, and custom theme files.

Summary:

- **`~/.arbiter/tui.json`** — `"preset": "nord"` or `"theme_file": "themes/mine.json"`, plus optional per-token overrides.
- **`themes/*.json`** (in the repo; **embedded into the binary** at build time) — all built-in presets; also written to `~/.arbiter/themes/` on `arbiter --init`.
- **`~/.arbiter/themes/*.json`** — your custom themes or edited copies of built-ins.
- **`arbiter --export-theme PRESET`** — dump a complete theme JSON to stdout (starter for editing).
- **`/theme`**, **`/theme save`**, **`/theme file`** — browse with ↑↓ preview, export, or load themes in-session.

Built-in presets:

| Preset | Character |
|--------|-----------|
| `high-contrast` | **Default.** High-contrast dark — white on black, saturated accents. |
| `onedark` | Atom OneDark — blue focus, warm code, green/red diffs. |
| `modern` | Neutral black chrome with warm orange accent. |
| `nord` | Cool arctic blues and muted frost tones. |
| `dracula` | Purple/pink/cyan on `#282a36`. |
| `solarized` | Ethan Schoonover Solarized Dark. |
| `light` | Light background for bright terminals. |
| `gruvbox` | Warm retro groove — orange/green on earthy browns. |
| `catppuccin` | Mocha pastels — lavender and pink on deep plum. |
| `tokyo-night` | Night city blues and soft purple accents. |
| `monokai` | Classic editor — yellow/green on olive black. |
| `rose-pine` | Muted rose and iris on midnight violet. |
| `ayu` | High-contrast dark — orange and cyan pops. |
| `cobalt` | Deep navy chrome with electric blue focus. |
| `everforest` | Forest greens and soft sage on charcoal. |
| `github` | GitHub-dark neutrals with blue links. |
| `palenight` | Material purple and soft blue-gray panels. |
| `synthwave` | Neon magenta and cyan on ultraviolet black. |
| `zenburn` | Low-contrast olive-gray with sage accents. |
| `solarized-light` | Solarized Light — cream paper with teal/blue accents. |
| `github-light` | GitHub Light — white chrome, blue links, soft neutrals. |
| `catppuccin-latte` | Catppuccin Latte — lavender pastels on soft paper. |
| `gruvbox-light` | Gruvbox Light — warm parchment with earthy accents. |
| `rose-pine-dawn` | Rosé Pine Dawn — muted rose and iris on ivory. |
| `one-light` | Atom One Light — clean gray-white with blue focus. |
| `papercolor` | PaperColor Light — high-legibility print palette. |
| `flexoki-light` | Flexoki Light — ink-on-paper with restrained hues. |
| `high-contrast-light` | High-contrast light — black text on white, vivid accents. |
| `kanagawa` | Kanagawa Wave — ink blacks with soft gold and sea blues. |
| `oxocarbon` | Oxocarbon — IBM Carbon blacks with neon pink/cyan. |
| `night-owl` | Night Owl — deep navy with soft violet and mint. |
| `horizon` | Horizon — warm rose accents on charcoal violet. |
| `flexoki` | Flexoki Dark — near-black ink with earthy accents. |
| `poimandres` | Poimandres — cool teal focus on midnight blue. |
| `vesper` | Vesper — minimal black with peach and mint accents. |
| `moonlight` | Moonlight — soft indigo panels with pastel syntax. |
| `material` | Material Oceanic — blue-gray chrome, Material accents. |
| `andromeda` | Andromeda — cyan/magenta pops on slate. |
| `catppuccin-frappe` | Catppuccin Frappé — muted pastels on soft charcoal. |
| `catppuccin-macchiato` | Catppuccin Macchiato — balanced midnight pastels. |
| `tokyo-night-storm` | Tokyo Night Storm — stormier panels, same city blues. |
| `tokyo-night-light` | Tokyo Night Light — day-mode blues on cool gray paper. |
| `rose-pine-moon` | Rosé Pine Moon — rose/iris on cooler violet night. |
| `ayu-mirage` | Ayu Mirage — warm gold focus on slate mirage. |
| `ayu-light` | Ayu Light — orange/cyan pops on bright paper. |
| `kanagawa-dragon` | Kanagawa Dragon — ink blacks with muted gold. |
| `kanagawa-lotus` | Kanagawa Lotus — parchment with sea and plum accents. |
| `everforest-light` | Everforest Light — soft sage on cream paper. |
| `papercolor-dark` | PaperColor Dark — high-legibility print on charcoal. |
| `nightfox` | Nightfox — cool blue focus on deep navy. |
| `dawnfox` | Dawnfox — Rosé Pine–adjacent dawn warm light. |
| `dayfox` | Dayfox — bright warm light with bold accents. |
| `duskfox` | Duskfox — violet dusk with soft rose accents. |
| `nordfox` | Nordfox — Nightfox × Nord arctic blues. |
| `terafox` | Terafox — teal earth tones on deep green-black. |
| `carbonfox` | Carbonfox — IBM Carbon blacks with neon accents. |
| `iceberg` | Iceberg — cool blue-gray chrome, soft contrasts. |
| `sonokai` | Sonokai — vivid Motoko accents on dark gray. |
| `aura` | Aura — purple/mint neon on near-black. |
| `laserwave` | Laserwave — magenta synthwave on violet black. |
| `cyberdream` | Cyberdream — high-saturation neon on OLED black. |
| `cyberdream-light` | Cyberdream Light — neon accents on white. |
| `nightfly` | Nightfly — deep navy with soft violet and mint. |
| `moonfly` | Moonfly — near-black with bright pastel syntax. |
| `jellybeans` | Jellybeans — warm classic Vim palette. |
| `apprentice` | Apprentice — low-contrast muted dark. |
| `gotham` | Gotham — teal-forward noir chrome. |
| `srcery` | Srcery — high-contrast earth and crimson. |
| `tender` | Tender — soft olive/cream accents on dark. |
| `tomorrow-night` | Tomorrow Night — classic Base16 dark. |
| `tomorrow` | Tomorrow — classic Base16 light. |
| `oceanic-next` | Oceanic Next — slate ocean with soft pastels. |
| `shades-of-purple` | Shades of Purple — vivid purple chrome, neon pops. |
| `cobalt2` | Cobalt2 — Wes Bos yellow focus on deep blue. |
| `panda` | Panda Syntax — pink/mint accents on charcoal. |
| `noctis` | Noctis — cool teal focus on deep blue-green. |
| `bluloco` | Bluloco Dark — saturated workbench accents. |
| `bluloco-light` | Bluloco Light — same accents on bright paper. |
| `doom-one` | Doom One — Emacs Doom dark, OneDark-adjacent. |
| `doom-one-light` | Doom One Light — Emacs Doom light variant. |
| `modus-vivendi` | Modus Vivendi — WCAG-oriented high-contrast dark. |
| `modus-operandi` | Modus Operandi — WCAG-oriented high-contrast light. |
| `seoul256` | Seoul256 — low-contrast warm gray. |
| `lucario` | Lucario — blue slate with purple/cyan pops. |
| `miasma` | Miasma — swampy olive/brown noir. |
| `zenbones` | Zenbones — muted stone with soft rose accents. |
| `fairy-floss` | Fairy Floss — pastel candy on soft purple. |
| `outrun` | Outrun — hot pink/cyan neon on deep purple. |
| `monokai-pro` | Monokai Pro — refined Monokai spectrum. |
| `monokai-light` | Monokai Light — classic Monokai on cream. |
| `github-dimmed` | GitHub Dimmed — softer GitHub Dark neutrals. |
| `quiet-light` | Quiet Light — gentle gray-white workbench. |
| `winter-is-coming` | Winter is Coming — icy blues on deep navy. |
| `abyss` | Abyss — deep blue-black, restrained accents. |
| `kimbie-dark` | Kimbie Dark — warm brown with orange focus. |
| `tomorrow-night-blue` | Tomorrow Night Blue — bright pastels on navy. |
| `tomorrow-night-bright` | Tomorrow Night Bright — high-contrast on black. |
| `spacegray` | Spacegray — cool gray chrome with soft blues. |
| `paraiso-dark` | Paraiso Dark — purple-brown Base16 dark. |
| `paraiso-light` | Paraiso Light — purple-brown Base16 light. |

Pick a preset in `tui.json`:

```json
{
  "preset": "nord"
}
```

Export and customize:

```bash
arbiter --export-theme nord > ~/.arbiter/themes/my-nord.json
```

Then set `"theme_file": "themes/my-nord.json"` or edit colors inline — see [themes.md](themes.md).
