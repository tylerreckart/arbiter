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
| Switch conversation (`Ctrl-w b` → Enter) | Focused pane's conversation is saved; selected thread attaches to that pane only. Other panes and the split layout stay put. |
| `/quit` / Ctrl-D  | Every distinct open-pane conversation is written to disk. Pane layout is **not** saved. |
| `/reset [agent]`  | Clears the named (or focused) agent's history in memory only. The next save snapshots the new state. |

Saves are not incremental — `/quit` and conversation switches write the full snapshot. A hard kill (`SIGKILL`, terminal close, power loss) loses any history accumulated since the last save.

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

Arbiter preserves the full conversation history it has for each agent. It does
not summarize, trim, or rewrite old turns before sending a model request.
Context-window behavior is delegated to the selected model provider. If a
conversation outgrows the provider's context handling, use `/reset [agent]` to
clear that agent's history.

## Sessions vs the structured memory graph

The conversation file is per-thread continuity. The **structured memory graph** (HTTP API only — `POST /v1/memory/entries`, FTS-ranked search, typed nodes + relations, temporal validity windows) is per-tenant durable knowledge. Two different tools:

- Conversation: "what did we just talk about in this thread" — restored when you switch back.
- Memory graph: "what facts has the agent recorded over time" — queried explicitly via `/v1/memory/entries?q=…`.

The TUI's `/mem write` writes to the per-agent scratchpad (a flat file), not the memory graph. The graph is the API surface for richer retrieval; see [`docs/concepts/structured-memory.md`](../concepts/structured-memory.md).
