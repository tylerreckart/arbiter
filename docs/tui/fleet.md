# Fleet dashboard

The fleet pane is a live tree of every agent turn in the current job — master, `/agent` / `/parallel` children, and reconcile JIT workers. It consumes the existing [fleet streaming](../concepts/fleet-streaming.md) SSE model (`stream_id` + `depth`); it is not a second orchestrator.

Presence (`◎ presence`) is the opposite lifetime: residents stay in the room. Fleet rows are request-scoped and go away when the job ends (JIT clones disappear on `agent.teardown`).

## When it appears

The pane occupies the **right rail** (~24–28 columns) once a job has at least one stream, the same width breakpoint as the session sidebar (96 remaining columns). Unlike the session sidebar, it **stays up in multi-pane layouts** — that is when you need it.

Idle sessions keep the rail free so conversation panes are not crowded. An empty Fleet box is not shown unless you force focus with `Ctrl-w f`.

When both Fleet and Session want the rail (single pane, wide terminal, a job is live), Fleet stacks on top and Session sits below.

## What each row shows

```
╭ Fleet ──────────────╮
│ ensure · wave 2     │
│ Δ 3 residual · 2 held│
│                     │
│ ● index        fetch│
│ ├ ● researcher_a 1.2k│
│ └ ◇ writer          │
╰─────────────────────╯
```

- **Indent** follows `depth` (0 master, 1 delegated, 2 sub-sub). The depth cap is 2; the tree does not invent a third level.
- **Status:** `●` running, `✓` ok, `✗` failed, `○` opened but not yet started.
- **Name** of the agent. Reconcile JIT workers get a `◇` marker (ephemeral clone).
- **Active tool** while a `/cmd` is in flight; otherwise **token totals** for that stream.

The overlay at the top is reconcile-only: `mode=ensure` residual vs held clause counts and the current wave, next to spawn/teardown rows. The pane does not run reconcile (that is still `POST /v1/reconcile`); it only displays events.

## Steer / focus

Selecting a row focuses the conversation pane bound to that stream so you can watch its output. Bindings follow the existing pane / sidebar conventions:

| Input | Effect |
|-------|--------|
| `Ctrl-w f` | Show the fleet pane and enter it (second press hides). |
| Click a fleet row | Select that agent and focus its pane. |
| `↑` / `↓` (or `k` / `j`) | Move the selection (while fleet is focused). |
| `Enter` | Focus / open the pane bound to the selected stream. |
| `Esc` | On a **running** row: cancel that pane's in-flight turn (existing kill-switch). Otherwise leave fleet focus. |
| Wheel over the tree | Scroll the list. |

`/parallel` children stream into the **host** pane (interim `→ agent` headers). Selecting them focuses that host — the dashboard does not auto-split a pane per child (that would drown the layout). `/pane` spawns already have their own leaf; selecting that agent focuses the spawned pane.

Cancel does **not** invent a per-stream kill protocol. Esc maps to the same `cancel_pane_turn` path as interrupting a focused conversation pane (local `CancelToken` or remote `POST /v1/requests/:id/cancel`).

## Event → tree

| Event | Tree |
|-------|------|
| `stream_start` | Open a row (`stream_id`, `depth`, agent). |
| `agent.spawned` | Open a JIT row (`clone_id`, `◇`). |
| `agent_start` / `text` | Mark running. |
| `tool_call` | Set / clear the active tool. |
| `token_usage` | Add input + output on that stream. |
| `reconcile.delta` | Overlay: residual, held, wave. |
| `stream_end` | Mark ended; keep the row until the next job. |
| `agent.teardown` | Remove the JIT row immediately. |
| `done` / new `request_id` | End leftovers / clear the previous job. |

See [SSE events](../concepts/sse-events.md) and [Fleet streaming](../concepts/fleet-streaming.md).

## See also

- [Panes](panes.md) — split / focus / `/pane`
- [Keybindings](keybindings.md)
- [Streaming](streaming.md)
- [Presence](../concepts/presence.md) — always-on residents
- [Reconcile](../concepts/reconcile.md) — JIT ΔS waves
