# Sessions

A session captures the *agents' memory of the conversation* — every message exchanged. Restoring a session means the agent picks up the conversation where it left off; the **rendered scrollback** (what you saw painted on screen) is not preserved.

## Global conversations

The TUI stores **multiple conversations globally** (not per working directory). Each conversation has its own agent message histories and title. Use the left-hand **conversation sidebar** (`Ctrl-w b`) to switch threads or start a new one.

Storage layout:

```
~/.arbiter/conversations/
  manifest.json     # [{id, title, cwd, created_at, updated_at}]
  active            # UUID of the last-open conversation
  <uuid>.json       # agent histories (index + loaded agents)
```

On first launch after upgrading, legacy per-cwd session files under `~/.arbiter/sessions/*.json` are imported into the global store automatically. Legacy files are left in place; new saves go only to `conversations/`.

## When sessions save and load

| Event             | What happens                                                       |
|-------------------|--------------------------------------------------------------------|
| `arbiter` startup | The last active conversation is loaded. Agent histories are restored before the first prompt. |
| After each completed turn | Background autosave (`save_async`) writes that pane's conversation. Distinct conversation ids (multi-pane) each keep a pending slot. |
| Periodic tick | Dirty conversations are flushed every 30s by default (`ARBITER_AUTOSAVE_INTERVAL_SEC`; `0` disables the timer). |
| `/reset` / `/compact` | History/compaction changes are queued for autosave immediately. |
| Switch conversation (`Ctrl-w b` → Enter) | Focused pane's conversation is flushed and saved; selected thread attaches to that pane only. Other panes and the split layout stay put. |
| `/quit` / Ctrl-D  | Pending autosaves drain, then every distinct open-pane conversation is written to disk. Pane layout is **not** saved. |

Saves write a full conversation snapshot (not an incremental journal). A hard kill (`SIGKILL`, terminal close, power loss) can still lose an **in-flight** turn; completed turns and dirty state older than the autosave interval should already be on disk.

## What's in a conversation file

Each `<uuid>.json` is a snapshot of the orchestrator's agent histories:

- **Index master history** — messages for the default `index` agent.
- **Loaded agent histories** — any sub-agents that had non-empty history when saved.

Per-agent scratchpads (`/mem write`) live in `~/.arbiter/memory/<agent>/notes.md` independent of any conversation — they survive across conversation switches.

Conversation **titles** are auto-generated from the first exchange (visible in the header and sidebar). Titles are stored in `manifest.json`.

## What's not persisted

- **Pane layout.** Restarting always opens a single pane. Switching conversations no longer collapses splits — only the focused pane rebinds.
- **Scrollback.** On relaunch the painted history is gone (conversation switch replays a transcript tail into the pane). Type a follow-up and the agent answers in context.
- **In-flight turns.** A turn streaming when you quit or switch is dropped — the partial response isn't in the saved history.
- **Background loops.** `/loop`-spawned processes are killed on exit.
- **Queue depth.** Any queued user inputs are dropped on exit.

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
when the window is unknown). Use `/compact [agent]` to force it. `/reset`
clears both history and compaction state for that agent.

The summary call uses `constitution.advisor.model` when set, otherwise the
executor model. Failures are fail-open: the turn proceeds with the uncompacted
view and a warning is logged.

## Sessions vs the structured memory graph

The conversation file is per-thread continuity. The **structured memory graph** (HTTP API only — `POST /v1/memory/entries`, FTS-ranked search, typed nodes + relations, temporal validity windows) is per-tenant durable knowledge. Two different tools:

- Conversation: "what did we just talk about in this thread" — restored when you switch back.
- Memory graph: "what facts has the agent recorded over time" — queried explicitly via `/v1/memory/entries?q=…`.

The TUI's `/mem write` writes to the per-agent scratchpad (a flat file), not the memory graph. The graph is the API surface for richer retrieval; see [`docs/concepts/structured-memory.md`](../concepts/structured-memory.md).
