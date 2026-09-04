# Presence (always-on agents)

Presence is a constitution-declared **residency class**. An always-on agent stays loaded and sits beside whoever is working — a pair colleague looking over the shoulder. After a tool batch, it may inject a short note that helps finish the task at hand, or stay silent.

The stance is pairing, not review. Jules (or any presence agent) is not specialized in programming unless the active agent is programming: editor at the elbow for prose, fellow analyst for research, teammate who remembers the house setup for ops.

This is the opposite lifetime of [JIT / reconcile](reconcile.md): JIT agents spawn for a $\Delta S$ slice and tear down when the clause is satisfied. Presence agents **stay in the room**.

## Why a runtime-owned resident

Today's supervision and memory surfaces fire at the wrong time, or on the wrong subject, for "I am watching what they are doing":

| Primitive | When it runs | What it sees | What it can do |
|-----------|--------------|--------------|----------------|
| [Advisor gate](advisor.md) | Terminating turn | Task + last text + tool summary | `CONTINUE` / `REDIRECT` / `HALT` |
| Advisor consult | Executor asks | The question only | Prose advice |
| [Lessons](lessons.md) / [todos](todos.md) | Pre-turn | Prompt / open list | Static preamble |
| [Loops](../tui/streaming.md) | Dedicated thread | The loop's own work | Self-prompt the same agent |
| [Event routing](../api/events.md) | `POST /v1/events` | Event payload | Start a *new* turn |
| **Presence** | After a peer's tool batch | Live snapshot of that work | Inject context into *that* turn |

The missing law is peer observation plus mid-turn injection. The runtime owns the checkpoint and the envelope — the working agent cannot opt out of being watched, and a watcher cannot take over the turn.

## Configuration

The `presence` block lives on a `Constitution`:

```jsonc
// Modern object form.
"presence": {
  "mode": "always_on",
  "watch": ["*"],
  "interject": "context",
  "model": "anthropic/claude-haiku-4-5",
  "max_notes_per_turn": 1
}

// String shorthand — equivalent to {mode: "always_on"}.
"presence": "always_on"
```

Absent / `"off"` (the default) keeps today's request-scoped specialist.

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `mode` | string | `"off"` | `"off"` or `"always_on"`. Unknown values warn and fall back to `"off"`. |
| `watch` | array\<string\> | `[]` | Agent-id globs (`fnmatch`). Empty ≡ every peer. A watcher never reviews itself. |
| `interject` | string | `"context"` | `"context"` runs the review and may inject. `"off"` declares residency but skips the model call — a kill switch. |
| `model` | string | watcher's `model` | Optional cheaper / different model for the review call. |
| `prompt` | string | built-in | Override the review system prompt. Capped at 8 KiB. |
| `max_notes_per_turn` | int | `1` | Cap on `CONTEXT` notes this watcher may inject per working-agent `stream_id`. Clamped to 1–4. |

The bundled [`jules`](../../agents/jules.json) starter is the canonical example: cheap model, `watch: ["*"]`, silent unless a note would save the active agent a step. Jules does **not** grade the work or take the keyboard.

## Signal grammar

The review is history-less — one snapshot in, one signal out — matching the advisor gate. Tag-based, case-insensitive on the signal token:

```
<signal>SILENT</signal>
```

```
<signal>CONTEXT</signal>
<note>one or two sentences the working agent should see now</note>
```

`SILENT` is the default. Surrounding prose is tolerated. Missing `<signal>`, an unknown token, or `CONTEXT` without `<note>` is **malformed Silent** (fail-open). Presence cannot `HALT` or `REDIRECT` — that remains the advisor's job.

## Runtime control flow

Where presence fires inside the dispatch loop:

1. The working agent emitted one or more writs and the runtime assembled the tool-result envelope.
2. The runtime collects loaded agents whose `presence.mode` is `always_on`, `interject` is not `off`, and `watch` matches the working agent id. Self is skipped. At most **3** watchers run per checkpoint.
3. Each watcher gets a structured snapshot:
   - watcher id / name / goal / rules
   - working agent id + role
   - original user task (capped at 16 KB)
   - the working agent's most recent assistant text (16 KB)
   - a one-line-per-call tool summary, same shape as the advisor gate (8 KB)
4. A transport or parse error becomes `SILENT`. A watcher never blocks the working agent.
5. Each `CONTEXT` note is prepended to the tool-result envelope as:

   ```
   [PRESENCE: Jules]
   Scout already fetched the v2 contract — reuse that instead of searching again.
   [END PRESENCE]
   ```

   The working agent's next model call sees the note above the tool results, the same way `[LOOP DETECTED]` and `[OPEN TODOS]` already inject.
6. Cost is attributed to the **watcher** (its review model), not the working agent.

Presence does **not** fire on a terminating turn with no tools — that is the advisor gate's window. It does not occupy a `LoopManager` thread. "Always-on" means **resident and subscribed**, not "always calling the model."

## Observability

| Surface | Behaviour |
|---------|-----------|
| SSE `presence` | One event per watcher consult: `kind` is `silent` or `context`; `watcher`, `agent` (the working agent), `stream_id`, optional `detail` / `malformed`. |
| TUI | `CONTEXT` paints `◎ presence · <watcher> <note>`. `SILENT` is quiet. |
| Roster | Always-on agents show `+presence` on the master's `AGENTS` line so index does not treat them as ordinary specialists. |

## Presence is not a second advisor

The advisor is bound to **the same agent** and answers *may this turn return?* Presence is a **colleague in the room** and answers *can I save them a step on this task?*

| | Advisor | Presence |
|---|---|---|
| Stance | Supervisor / gate | Pair colleague at the shoulder |
| Subject | The executor it is configured on | A *peer* (never self) |
| When | Terminating turn (`cmds.empty()`) | After that peer's tool batch |
| Power | `CONTINUE` / `REDIRECT` / `HALT` | `SILENT` / `CONTEXT` only |
| Failure | Fail-closed by default | Fail-open (`SILENT`) |
| Job | Judgment — is the work acceptable? | Context that helps finish the task |

If you would halt, redirect, or score the turn, that is the [advisor](advisor.md). If you would lean over and say “we already have that” or “the ask also wanted X,” that is presence.

A safety-cop resident (flag footguns, retries, dropped constraints as a verdict) *looks* like presence but is a weaker gate. Do not use the class that way. Pair them: presence helps mid-loop; the gate still owns the terminating verdict.

## What this is not

- **Not a second advisor.** See above. Pair with `advisor.mode: "gate"` when you need a hard halt.
- **Not a loop.** `/loop` still exists for "keep doing this." Presence watches *someone else*.
- **Not JIT.** Reconcile still owns spawn-for-delta / teardown-on-satisfy. A catalog can hold both classes.
- **Not cross-request in v1.** A watcher sees peers inside the same orchestrator (the in-flight turn and its `/agent` / `/parallel` children). Cross-request observation over `RequestEventBus` is a later hub.

## See also

- [Advisor](advisor.md) — terminating-turn supervision.
- [Reconcile](reconcile.md) / [JIT PRD](https://github.com/tylerreckart/arbiter/issues/208) — the opposite lifetime.
- [SSE events](sse-events.md) — `presence` payload.
- [Agent data model](data-model.md) — where the block lives.
- [`POST /v1/agents`](../api/agents/create.md) — create a stored presence agent.
