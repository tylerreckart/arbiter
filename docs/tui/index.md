# Arbiter TUI

Arbiter's interactive terminal interface. One process, one shell window — but inside that window: a multi-pane layout where each pane is an independent conversation with its own agent, history, and streaming output. The TUI is what you get when you run `arbiter` with no arguments.

Rendering uses [OpenTUI](https://github.com/anomalyco/opentui) (native cell-buffer diff renderer). Per-pane chrome, scrollback, and input are composited each frame by the output pump (~30 ms).

A pane is to a conversation what a tab is to a browser. A new pane is a new conversation against an agent of your choosing; multiple panes run side-by-side or stacked, each independently typing, streaming, and waiting for its agent. Background loops (long-running agent processes) live alongside; foreground panes can spawn child panes (`/pane <agent> <msg>`) whose results land back in the spawner when done.

Start with `arbiter`. The default layout is a single pane covering the main terminal area. A **left-hand conversation sidebar** lists prior threads and lets you start a new one (`Ctrl-w b` to enter, `Ctrl-w t` to toggle visibility). A **right-hand sidebar** shows session usage, the focused pane's active task, and recent tool/MCP activity when the terminal is wide enough; `Ctrl-w s` toggles it. `Ctrl-w v` / `Ctrl-w h` split the main area.

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
- **Hint row.** Static legend of the most-used keys and commands. Hidden in multi-pane layouts (becomes clutter on every pane); the rows are still reserved as blank padding so input row position doesn't shift between modes.

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
- **[Sessions](sessions.md)** — history persistence, session restore on relaunch, per-cwd scoping.
- **[Output styling plan](output-styling-plan.md)** — analysis of the render pipeline and a phased plan for spacing, surfaces, and StyleId unification.

## Configuration

Per-user state lives under `~/.arbiter/`:

| Path                       | What it is                                              |
|----------------------------|---------------------------------------------------------|
| `openrouter_api_key`       | OpenRouter API key for hosted models.                   |
| `agents/*.json`            | Agent constitutions — one file per agent.               |
| `sessions/*.json`          | Per-cwd session snapshots (auto-saved on `/quit`).      |
| `memory/<agent>/*.md`      | Per-agent persistent scratchpads (`/mem write`).        |
| `tenants.db`               | Tenant identity store. Only used when running `--api`.  |

Everything else (current pane layout, scrollback, in-flight turns) is in-memory only and gone when the process exits — except for the session snapshot, which is restored automatically the next time you `arbiter` from the same directory.

## TUI design config

Themes are JSON-driven. See **[themes.md](themes.md)** for the full schema, export workflow, and custom theme files.

Summary:

- **`~/.arbiter/tui.json`** — `"preset": "nord"` or `"theme_file": "themes/mine.json"`, plus optional per-token overrides.
- **`themes/*.json`** (in the repo / `share/arbiter/themes/` when installed) — all built-in presets as editable JSON; copied to `~/.arbiter/themes/` on `arbiter --init`.
- **`~/.arbiter/themes/*.json`** — your custom themes or edited copies of built-ins.
- **`arbiter --export-theme PRESET`** — dump a complete theme JSON to stdout (starter for editing).
- **`/theme`**, **`/theme save`**, **`/theme file`** — switch, export, or load themes in-session.

Built-in presets:

| Preset | Character |
|--------|-----------|
| `onedark` | **Default.** Atom OneDark — blue focus, warm code, green/red diffs. |
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
