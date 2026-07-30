# Sessions

A session captures the *agents' memory of the conversation* — every message exchanged. Restoring a session means the agent picks up the conversation where it left off; the **rendered scrollback** (what you saw painted on screen) is not preserved.

## Global conversations

The TUI stores **multiple conversations globally** (not per working directory). Each conversation has its own agent message histories and title. Use the left-hand **conversation sidebar** (`Ctrl-w b`) to switch threads or start a new one.

Storage layout:

```
~/.arbiter/conversations/
  manifest.json     # [{id, title, cwd, created_at, updated_at}]
  active            # UUID of the last-open conversation
  layout.json       # multi-pane tree + per-pane conversation ids
  <uuid>.json       # agent histories (index + loaded agents)
```

On first launch after upgrading, legacy per-cwd session files under `~/.arbiter/sessions/*.json` are imported into the global store automatically. Legacy files are left in place; new saves go only to `conversations/`.

## When sessions save and load

| Event             | What happens                                                       |
|-------------------|--------------------------------------------------------------------|
| `arbiter` startup | The last layout snapshot is restored when valid (split tree + per-pane conversation ids); each open conversation's agent history is loaded. A transcript tail is replayed once per `(conversation_id, agent)` binding (pre-order first leaf wins) so empty sibling windows that share a conversation stay empty, matching live `^W` splits. Missing snapshot → single pane on the active conversation. |
| After each completed turn | Background autosave (`save_async`) writes that pane's conversation. Distinct conversation ids (multi-pane) each keep a pending slot. |
| Mid-turn checkpoints | After each successful model iteration and after each tool-result envelope is committed to history, `save_async` runs so completed tool work survives quit/cancel/SIGKILL. |
| Periodic tick | Dirty conversations are flushed every 30s by default (`ARBITER_AUTOSAVE_INTERVAL_SEC`; `0` disables the timer). |
| `/reset` / `/compact` | History/compaction changes are queued for autosave immediately. |
| Switch conversation (`Ctrl-w b` → Enter) | Focused pane's conversation is flushed and saved; selected thread attaches to that pane only. Other panes and the split layout stay put. Layout snapshot is rewritten. |
| Split / close / focus / separator drag | `layout.json` is rewritten so relaunch matches the live tree (including asymmetric weights). |
| `/quit` / Ctrl-D  | Pending autosaves drain, every distinct open-pane conversation is written to disk, then the pane layout snapshot is saved. |

Saves write a full conversation snapshot (not an incremental journal). A hard kill can still lose an **unfinished** model stream (tokens not yet committed as an assistant message). Completed tool-result envelopes are committed to history and checkpoint-saved before the next LLM wait.

## What's in a conversation file

Each `<uuid>.json` is a snapshot of the orchestrator's agent histories:

- **Index master history** — messages for the default `index` agent.
- **Loaded agent histories** — any sub-agents that had non-empty history when saved.

Per-agent scratchpads (`/mem write`) and the structured memory graph (`/mem search|entries|entry|add …`) live in `~/.arbiter/tenants.db` (same store the API uses), independent of any conversation file — they survive across conversation switches.

Conversation **titles** are auto-generated from the first exchange (visible in the header and sidebar). Titles are stored in `manifest.json`.

## What's not persisted

- **Scrollback pixels.** On relaunch the painted history is rebuilt from a transcript tail replay (same as conversation switch), not from a pixel buffer.
- **Zoom.** `Ctrl-w z` is session-local; relaunch restores the un-zoomed split tree.
- **Unfinished model streams.** Assistant text still being streamed when you quit is not committed; prior iterations and completed tool-result envelopes from the same turn are.
- **Background loops.** `/loop`-spawned processes are killed on exit.
- **Queue depth.** Any queued user inputs are dropped on exit.

Deleted conversations referenced by `layout.json` are remapped to the active conversation on restore. Corrupt or oversized snapshots are ignored and the TUI starts with a single pane.

## Cleaning up

Use the sidebar to start fresh (`+ New conversation`) or delete individual files under `~/.arbiter/conversations/`. `/reset` only clears history in memory for the active conversation.

To purge everything: `rm -rf ~/.arbiter/conversations/`.

## Context Length

Arbiter keeps the **full** conversation history on disk and in memory (for
transcript replay and session restore). Separately, before each model request
it may build a compacted *view*: a rolling summary of older turns plus a recent
message window.

Compaction triggers automatically when the last turn's prompt tokens reach
~75% of the model's known context window (or an approximate character budget
when the window is unknown). Override with `ARBITER_COMPACT_THRESHOLD`
(0–1) or disable autos with `ARBITER_COMPACT_DISABLED=1`. Use `/compact [agent]`
to force it. `/reset` clears both history and compaction state for that agent.

The summary call uses `constitution.advisor.model` when set, otherwise the
executor model. Failures are fail-open: the turn proceeds with the uncompacted
view and a warning is logged.

## Sessions vs the structured memory graph

The conversation file is per-thread continuity. The **structured memory graph** (typed nodes + relations in `tenants.db`, FTS-ranked search, temporal validity windows) is per-tenant durable knowledge. Both the TUI (`/mem search|entries|entry|expand|density|add entry|add link|invalidate`) and the HTTP API (`/v1/memory/*`) share that store. Two different tools:

- Conversation: "what did we just talk about in this thread" — restored when you switch back.
- Memory graph: "what facts has the agent recorded over time" — queried via `/mem …` in the TUI or `/v1/memory/entries?q=…` over HTTP.

`/mem write` appends to the per-agent **scratchpad** (`agent_scratchpad` in `tenants.db`), not the graph. Use `/mem add entry … /endmem` for graph nodes. See [`docs/concepts/structured-memory.md`](../concepts/structured-memory.md).
