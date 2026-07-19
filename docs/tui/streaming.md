# Streaming and turn lifecycle

What you see on screen during a turn — from sending a message to the agent finishing its reply. Three indicators surface progress without cluttering the scroll region: the **thinking** spinner (header), the **tool-call** spinner (mid separator), and `[interrupted]` markers (cancel path).

## Sending a message

Plain text in the input area, hit Enter. The line is pushed onto the focused pane's command queue and the prompt clears. If a previous turn is still streaming, the new line **queues** — the input row's status briefly shows `queued (N)` so you know the buffer is filling.

Queueing means you can keep typing while the agent works; subsequent inputs run in order without you waiting for the spinner.

## During the turn

The exec thread picks the message off the queue and starts the orchestration loop. Three phases play out visibly:

### 1. Thinking

While waiting for the first byte from the model, the header status (right side of row 1) animates `thinking` with a Braille spinner:

```
agent · my session                                  ⠋ thinking…
```

This indicator runs in the output pump and updates at ~80 ms cadence. It clears the moment the first content chunk arrives. Token / cost stats from the previous turn stay hidden behind it; they re-appear when the turn ends.

### 2. Streaming output

Model deltas land in the scroll region as they arrive via `StreamRenderer`: `BlockParser` suppresses `/cmd` lines (unless `/verbose`), `MarkdownRenderer` emits styled spans into `ProseSegment`, non-diff fenced code streams into `CodeSegment` (draw-time collapse — first eight body lines plus a live `… +N lines` summary, no placeholder artifacts in scrollback), and ` ```diff ` patches become side-by-side `DiffSegment` panels. Styling resolves from `tui.json`'s `content` tokens through `StyleId` → OpenTUI highlights — no ANSI round-trip on the master stream path. User echoes, advisor/loop banners, and interrupt markers use the same prose path. Vertical rhythm (`block_gap` / `panel_gap` / `prose_paragraph_gap`) and panel surfaces (`code_bg`, `diff_bg_*`) are theme tokens — see [output-styling-plan.md](output-styling-plan.md).

Sub-agent progress uses the same pipeline with `kInterim` policy: dimmed prose, collapsed fences, capped at eight display rows / 480 columns. The master's synthesis turn carries the full result.

### 3. Tool calls

When the agent emits `/fetch`, `/exec`, `/agent`, `/mem`, `/write … /endwrite`, etc., `BlockParser` intercepts those lines. Behaviour depends on `/verbose`:

**Verbose off (default).** The raw `/cmd` lines are *swallowed* from the scroll region. Each tool still appears as a compact `ToolSegment` row in scrollback (`○ exec:git status` → `✓` / `✗`), and the mid separator above the input shows a live count spinner:

```
─── [⠋ 3 tool calls…] ───────────────────────────────────────
```

`^O` expands the focused tool row to show args and a truncated result preview. The model's prose between `/cmd` blocks renders normally; only the bare `/cmd` lines (and `/write` bodies) stay hidden. See [output-ux.md](output-ux.md).

**Verbose on.** Every `/cmd` line, every tool result, every `/write … /endwrite` block streams into scroll (in addition to the tool rows). Useful when debugging an agent's tool use; noisy for normal interaction. Toggle with `/verbose on` / `/verbose off`.

The thinking and tool-call spinners deliberately don't share a row — early versions both fought for row 1 at 80 ms cadence and produced visible flashing. Header is for "agent is generating"; mid separator is for "agent is calling tools."

When a provider streams a separate reasoning channel (`thinking_delta` /
`reasoning_content` / Gemini `thought` parts), a collapsed **thinking** row
also appears in scrollback above the assistant prose. Expand with `^O`.
Models without that channel keep the header spinner only.

## End of turn

When the model emits its stop signal, three things happen in order:

1. The thinking indicator stops; the status row shows the new aggregate token / cost stats.
2. The tool-call indicator (if it ran) emits its summary line into scrollback and clears the mid separator back to plain dashes.
3. The input row redraws fresh; `begin_input` parks the cursor for the next message and surfaces `queued (N)` if more inputs are waiting.

Scroll position is preserved across turns — if you PgUp'd to read history while the turn was streaming, the live tail keeps appending behind you. The header status shows `↑ N lines above` so you know fresh content is accumulating; PgDn at any time snaps you back to the live tail.

## Cancelling

Press `Esc` (alone, no follow-on within 50 ms) to cancel the focused pane's in-flight turn:

1. The cancel handler pushes a system activity line (`· [interrupted]`) into the scroll region.
2. The agent's pending HTTP request is aborted (libcurl `curl_multi_remove_handle`).
3. The exec thread unwinds back to the queue's `pop()`.
4. The input row redraws with whatever was queued next, or empty.

Cancellation is per-pane: siblings keep streaming undisturbed. If a tool call was in flight when you cancelled, that tool's result may or may not arrive depending on where the abort caught it — the agent's next turn won't see it either way.

## Background loops

`/loop <agent> <prompt>` spawns a long-running process: the loop manager invokes the agent repeatedly, buffers its output, and runs decoupled from any pane. The loop's stream does **not** appear in any pane by default — `/log <loop-id>` prints buffered output on demand, `/watch <loop-id>` tails it live in the focused pane (Enter to detach). This separation keeps loops from drowning your foreground panes.

Loops are useful for "watch this and report" patterns — a research agent polling a feed, a maintenance agent grinding through a queue. See `/loop`, `/loops`, `/inject`, `/suspend`, `/resume`, `/kill` in [commands.md](commands.md).

## What gets persisted

In-memory only:
- Scrollback (OpenTUI `TextBuffer` per pane).
- Tool-call counters.
- Pane layout.

Persisted across runs:
- Conversation history (the agent's context — message list).
- Per-agent scratchpads (`/mem write`).
- Session metadata (focused agent per pane, cwd association).

So restarting drops the rendered scrollback (you can't PgUp into yesterday's session) but keeps the **agent's memory of the conversation** — type a follow-up and the agent answers in context. See [sessions.md](sessions.md).
