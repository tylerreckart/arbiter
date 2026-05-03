# Arbiter TUI

Arbiter's interactive terminal interface. One process, one shell window — but inside that window: a multi-pane layout where each pane is an independent conversation with its own agent, history, and streaming output. The TUI is what you get when you run `arbiter` with no arguments.

A pane is to a conversation what a tab is to a browser. A new pane is a new conversation against an agent of your choosing; multiple panes run side-by-side or stacked, each independently typing, streaming, and waiting for its agent. Background loops (long-running agent processes) live alongside; foreground panes can spawn child panes (`/pane <agent> <msg>`) whose results land back in the spawner when done.

Start with `arbiter`. The default layout is a single pane covering the full terminal; `Ctrl-w v` / `Ctrl-w s` splits it.

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
- **Scroll region.** Where everything the agent emits goes — streamed prose, tool-call summary lines, `/cmd` output, system notices. Scrollable with PgUp/PgDn (visual-row aware: wrapped lines count as multiple rows for navigation).
- **Mid separator.** Dashed line above the input. Doubles as the tool-call indicator: while a turn is firing tool calls, this row shows `⠋ N tool calls…` instead of plain dashes.
- **Input area.** 1 row by default, grows up to 5 as readline wraps. Standard editing controls (arrows, history, tab-complete on slash commands).
- **Hint row.** Static legend of the most-used keys and commands. Hidden in multi-pane layouts (becomes clutter on every pane); the rows are still reserved as blank padding so input row position doesn't shift between modes.

## Where to next

- **[Slash commands](commands.md)** — the full `/cmd` catalogue, grouped by category.
- **[Keybindings](keybindings.md)** — every key, chord, and modifier the editor recognizes.
- **[Panes](panes.md)** — multi-pane layouts: split, focus, close, `/pane` spawn semantics.
- **[Streaming](streaming.md)** — what you see during a turn: thinking spinner, tool-call indicator, verbose mode, cancellation.
- **[Sessions](sessions.md)** — history persistence, session restore on relaunch, per-cwd scoping.

## Configuration

Per-user state lives under `~/.arbiter/`:

| Path                       | What it is                                              |
|----------------------------|---------------------------------------------------------|
| `api_key`                  | Anthropic API key (used by all agents that hit Claude). |
| `openai_api_key`           | OpenAI API key.                                         |
| `agents/*.json`            | Agent constitutions — one file per agent.               |
| `sessions/*.json`          | Per-cwd session snapshots (auto-saved on `/quit`).      |
| `memory/<agent>/*.md`      | Per-agent persistent scratchpads (`/mem write`).        |
| `tenants.db`               | Tenant identity store. Only used when running `--api`.  |

Everything else (current pane layout, scrollback, in-flight turns) is in-memory only and gone when the process exits — except for the session snapshot, which is restored automatically the next time you `arbiter` from the same directory.
