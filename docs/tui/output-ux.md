# Agent output UX

How the TUI presents agent turns as a first-class activity timeline.
Theme tokens and spacing live in [themes.md](themes.md).

## Turn anatomy

Live order within a turn (tools appear as `/cmd` lines are dispatched, after
the model stream that emitted them):

```
user echo strip (accent + vertical pad, input bg)
                                  ← one blank row
thinking  ▸                                ThinkingSegment header (when provider emits)
  first preview lines…                     up to 3 wrapped body rows when collapsed
                                  ← one blank row
assistant prose / markdown / code / diffs  (writ lines swallowed)
                                  ← one blank row
○ fetch:https://…                          ToolSegment appears as dispatch starts
✓ exec:git status  ▸                       resolves when the result returns
                                  ← one blank row
· [interrupted]                            activity chrome (system lines)
→ delegating: /agent …                     bold/info routing status (not prose)
› /read #2                                 verbose writs (› + WritLine)
```

| Layer | Segment | Notes |
|-------|---------|-------|
| User | `ProseSegment` + `UserEchoText` | Full-width strip with input accent + vertical pad |
| Tools | `ToolSegment` | One row per `/cmd`; expand for args/result; clustered (no gap between tools) |
| Reasoning | `ThinkingSegment` | Only when the provider streams a reasoning channel |
| Assistant | `Prose` / `Code` / `Diff` | Same StreamRenderer path as before |
| System | styled activity / delegation lines | Interrupts, advisor, `→ delegating:` |

Quiet default: writ lines stay swallowed (`BlockParser` covers `/read`, `/browse`,
`/todo`, `/search`, …); tools appear as compact status rows, not raw `/fetch`
dumps. `/verbose` still streams raw writs with `›` WritLine styling.

Blocks are separated by exactly one blank row (`layout.block_gap`, default 1).
Trailing soft blanks inside prose are trimmed before the next block so gaps
never stack to two.

## Tool timeline

`execute_agent_commands` emits `ToolActivityEvent` at **Started** and
**Finished** with process-wide unique ids. The REPL:

1. Upserts a `ToolSegment` in scrollback
2. Bumps the mid-separator spinner count on Finished (armed once per turn)
3. Records the Finished event onto the pane agent’s last assistant message as
   `tool_trace` (conversation switch replays that history). When a nested
   `/agent` dispatched the tool, the same entry is mirrored onto the child.

Expand/collapse: `^O` (same chord as code blocks) when a tool or thinking
row is in view.

## Permission cards

Destructive `/exec` and disk `/write` confirms render a multi-line card:

```
permission write  src/main.cpp
  12 lines, 340 bytes
  #include …
  …
  allow? [y/N]
```

Keys remain `y` / `n`. Declines still return `ERR: user declined` to the agent.

## Reasoning

When Anthropic emits `thinking_delta`, OpenAI-compat emits
`reasoning_content` / `reasoning`, or Gemini streams parts with
`"thought": true` (via `thinkingConfig.includeThoughts` on Gemini 2.5/3;
Flash-Lite also sets `thinkingBudget`), deltas land in a collapsed
`ThinkingSegment`. Models without a separate reasoning channel keep the
header **thinking…** spinner only — Arbiter does not invent chain-of-thought
from ordinary prose. Reasoning is stored on the assistant `Message.thinking`
field (including nested `/agent` and `/parallel` deltas appended onto the
pane agent’s open turn) and rebuilt on conversation switch.

## Replay

`transcript_replay` rebuilds `ThinkingSegment`s from `Message.thinking`,
runs assistant content through `StreamRenderer(kReplay)`, then rebuilds
`ToolSegment`s from each message’s `tool_trace` on the pane agent’s
history (typically `index`).

## Related

- [streaming.md](streaming.md) — turn lifecycle, spinners, cancel
- [themes.md](themes.md) — `TuiDesign` schema, theme tokens, spacing
