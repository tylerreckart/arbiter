# Sessions

A session captures the *agents' memory of the conversation* — every message exchanged. Restoring a session means the agent picks up the conversation where it left off; the **rendered scrollback** (what you saw painted on screen) is not preserved.

## Remote sessions (`--connect`)

`arbiter --connect <url> [--token …]` attaches the TUI to a remote [`arbiter --api`](../cli/api.md). Conversations, messages, and agents live on the server (HTTP `/v1/conversations`, SSE turns). The client does not read or write local `origin='tui'` rows for that session; the session sidebar shows `Remote · host`. See [`docs/cli/connect.md`](../cli/connect.md).

## Global conversations

The TUI stores **multiple conversations in a global index**, but each conversation is **bound to the workspace directory** where it was created (`conversations.cwd`). Host filesystem ops for that thread (`/write`, `/diff apply`) resolve under that bound root — not whatever directory you launched `arbiter` from. Switching chats switches the project root with them. Each conversation also has its own agent message histories and title. Use the left-hand **conversation sidebar** (`Ctrl-w b`) to switch threads or start a new one; the sidebar subtitle shows the bound project dirname.

TUI threads live in **`~/.arbiter/tenants.db`** — the same SQLite database the HTTP API uses for conversations, memory, todos, and schedules. Sidebar rows are `origin = 'tui'` conversation rows; the multi-agent session document (index + agents + compaction) is stored as `session_json` on that row. Active conversation id and the multi-pane layout also live in `tui_prefs` in the same DB (with a file mirror of `layout.json` for convenience).

### Folders

Conversations can be filed into **folders** (`conversation_folders` in `tenants.db`). The history sidebar shows a sectioned tree:

1. `+ New conversation`
2. **Folders** — each folder as a `[+]` / `[-]` header; expanded folders list their conversations nested underneath with a `│` guide
3. **Chats** — unfiled conversations (most-recently-updated)

Category headers are not selectable (↑/↓ skips them). Collapse state persists in `tui_prefs.folder_collapse_json` (JSON array of folder ids). Enter on a folder header toggles collapse; Enter on a conversation switches. `n` / `+ New` creates in the focused folder when the selection is a folder header or a child of a folder; otherwise unfiled.

Per-row menu (`m`): conversations get Open / Rename / Move to… / Delete; folders get Rename / New chat here / Delete folder (children are unfiled); `+ New` gets New folder. Press `f` anywhere in the sidebar to name a new folder. On narrow terminals use `/chat folder list|new|rename|delete|move` instead of the sidebar.

```
~/.arbiter/tenants.db
  conversations          # shared table; TUI rows have origin='tui' + session_json + folder_id
  conversation_folders   # named folders (position, name) per tenant
  tui_prefs              # active_conversation_id, layout_json, folder_collapse_json, …
~/.arbiter/conversations/   # legacy archive (imported once) + layout.json mirror
  manifest.json / <uuid>.json / active   # read once on upgrade, then ignored
  layout.json                            # file mirror of tui_prefs.layout_json
```

On first launch after this unification, any existing `~/.arbiter/conversations/` manifest + session files are imported into `tenants.db` automatically (layout leaf ids are rewritten). The legacy tree is left in place as an archive and is not re-imported.

Older per-cwd session files under `~/.arbiter/sessions/*.json` were already folded into the JSON conversation store in a prior migration; those imports ride along when the JSON store is lifted into SQLite.
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

## What's in a conversation session

Each TUI conversation's `session_json` is a snapshot of the orchestrator's agent histories:

- **Index master history** — messages for the default `index` agent.
- **Loaded agent histories** — any sub-agents that had non-empty history when saved.

Per-agent scratchpads (`/mem write`) and the structured memory graph (`/mem search|entries|entry|add …`) live in the same `tenants.db`, and are now scoped to the **active sidebar conversation** (same integer id as the TUI thread).

Conversation **titles** are auto-generated from the first exchange (visible in the header and sidebar). Titles, token totals, and the titled-lock flag are columns on the conversation row.
## What's not persisted

- **Scrollback pixels.** On relaunch the painted history is rebuilt from a transcript tail replay (same as conversation switch), not from a pixel buffer.
- **Zoom.** `Ctrl-w z` is session-local; relaunch restores the un-zoomed split tree.
- **Unfinished model streams.** Assistant text still being streamed when you quit is not committed; prior iterations and completed tool-result envelopes from the same turn are.
- **Background loops.** `/loop`-spawned processes are killed on exit.
- **Queue depth.** Any queued user inputs are dropped on exit.

Deleted conversations referenced by `layout.json` are remapped to the active conversation on restore. Corrupt or oversized snapshots are ignored and the TUI starts with a single pane.

## Cleaning up

Use the sidebar to start fresh (`+ New conversation`) or soft-delete / purge via the sidebar / `/chat delete`. `/reset` only clears history in memory for the active conversation.

To wipe TUI threads while keeping tenants/memory: delete `origin='tui'` rows from `tenants.db` (or remove the DB and re-import from a backup). The legacy `~/.arbiter/conversations/` archive is unused after migration.
## Context Length

Arbiter keeps the **full** conversation history on disk and in memory (for
transcript replay and session restore). Separately, before each model request
it may build a compacted *view*: a rolling summary of older turns plus a recent
message window.

Compaction triggers automatically when the last turn's prompt tokens reach
~75% of the model's known context window (or an approximate character budget
when the window is unknown). Override with `ARBITER_COMPACT_THRESHOLD`
(integer percent 1–100, default `75`) or disable autos with
`ARBITER_COMPACT_DISABLED=1`. Use `/compact [agent]`
to force it. `/reset` clears both history and compaction state for that agent.

The summary call uses `constitution.advisor.model` when set, otherwise the
executor model. Failures are fail-open: the turn proceeds with the uncompacted
view and a warning is logged.

## Sessions vs the structured memory graph

The conversation session is per-thread continuity. The **structured memory graph** (typed nodes + relations in `tenants.db`, FTS-ranked search, temporal validity windows) is per-tenant durable knowledge. Both the TUI (`/mem search|entries|entry|expand|density|add entry|add link|invalidate`) and the HTTP API (`/v1/memory/*`) share that store. Two different tools:

- Conversation: "what did we just talk about in this thread" — restored when you switch back.
- Memory graph: "what facts has the agent recorded over time" — queried via `/mem …` in the TUI or `/v1/memory/entries?q=…` over HTTP.

`/mem write` appends to the per-agent **scratchpad** (`agent_scratchpad` in `tenants.db`), not the graph. Use `/mem add entry … /endmem` for graph nodes. See [`docs/concepts/structured-memory.md`](../concepts/structured-memory.md).
