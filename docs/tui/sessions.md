# Sessions

A session captures the *agents' memory of the conversation* — every message exchanged, the active model per agent, per-pane scratchpad pointers. Restoring a session means the agent picks up the conversation where it left off; the **rendered scrollback** (what you saw painted on screen) is not preserved.

## When sessions save and load

| Event             | What happens                                                       |
|-------------------|--------------------------------------------------------------------|
| `arbiter` startup | The session for the current cwd is loaded if one exists. Conversation histories are restored to each agent before the first prompt. |
| `/quit` / Ctrl-D  | The current state is written to disk. Pane layout is **not** saved (always single-pane on next start). |
| `/reset [agent]`  | Clears the named (or focused) agent's history in memory only. The next save snapshots the new state. |
| `/compact [agent]` | Summarizes history to a single system message and clears the rest. The compaction is in-memory until next save. |

Saves are not incremental — `/quit` writes the full snapshot. A hard kill (`SIGKILL`, terminal close, power loss) loses any history accumulated since the last save.

## Per-cwd scoping

Session files are keyed by the current working directory's hash:

```
~/.arbiter/sessions/<cwd-hash>.json
```

`cwd` here is the directory you launched arbiter from. The same agents in two different project directories see two completely separate conversation histories — switch into `~/projects/foo` and the `index` agent has zero memory of yesterday's `~/projects/bar` session.

This scoping is the right default for multi-project work: each project gets its own pristine context, and you don't have to manage explicit "session names." If you want a single shared session across projects, launch arbiter from the same directory each time (e.g. `~`).

## What's in a session file

Each `<cwd-hash>.json` is a snapshot of the orchestrator's state:

- **Per-agent conversation history** — the full message list (user, assistant, tool result roles) the agent will see in its next turn's context.
- **Per-agent model override** — anything `/model <agent> <model-id>` set, so the next launch keeps the same model choice.
- **Per-agent token / cost ledger** — running totals for `/tokens` and the header status.
- **The shared scratchpad** — what `/mem shared` wrote during the session.
- **Session title** — auto-generated from the first message (visible in the header).

Per-agent scratchpads (`/mem write`) live in `~/.arbiter/memory/<agent>/notes.md` independent of any session — they're not in the session file, and they survive across cwd changes.

## What's not persisted

- **Pane layout.** Always single-pane on restart. If you had a 4-pane layout, you start over.
- **Scrollback.** The painted history is gone. Type a follow-up and the agent answers in context, but you can't PgUp to yesterday's session.
- **In-flight turns.** A turn streaming when you `/quit` is dropped — the partial response isn't in the saved history.
- **Background loops.** `/loop`-spawned processes are killed on exit. Restarting doesn't bring them back; you re-issue the loop commands.
- **Queue depth.** Any queued user inputs are dropped on exit.

## Cleaning up

Delete `~/.arbiter/sessions/<cwd-hash>.json` to start fresh from a directory. There's no UI for this in the TUI — `/reset` only clears history in memory and the next save overwrites the file.

To purge everything: `rm -rf ~/.arbiter/sessions/`.

## Compacting before saving

A long session's history grows unboundedly — at some point the next turn either hits the model's context limit or wastes tokens replaying old context the agent doesn't need. `/compact [agent]` solves this:

1. Asks the agent to summarize its history into one paragraph.
2. Replaces the full history with a single system message containing that summary.
3. The next turn sees the summary plus your fresh input — much shorter context, same continuity.

`/compact` is safe to run during a session; the `/quit` save afterwards is correspondingly smaller. There's no auto-compact heuristic.

## Sessions vs the structured memory graph

The session file is per-conversation continuity. The **structured memory graph** (HTTP API only — `POST /v1/memory/entries`, FTS-ranked search, typed nodes + relations, temporal validity windows) is per-tenant durable knowledge. Two different tools:

- Session: "what did we just talk about, in this directory" — restored automatically.
- Memory graph: "what facts has the agent recorded over time" — queried explicitly via `/v1/memory/entries?q=…`.

The TUI's `/mem write` writes to the per-agent scratchpad (a flat file), not the memory graph. The graph is the API surface for richer retrieval; see [`docs/api/concepts/structured-memory.md`](../api/concepts/structured-memory.md).
