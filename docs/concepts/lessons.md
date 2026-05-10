# Lessons (self-reflection)

A persistent record of what the agent has learned the hard way: *"this approach failed, here's what to try instead"*. Distinct from [structured memory](structured-memory.md) — memory holds **facts the agent observed**; lessons hold **the agent's own reflections on its mistakes**, scoped to the agent's identity rather than to a conversation.

The point: stop agents repeating the same failures across sessions. A `research` agent that figures out (the hard way) that a particular endpoint requires an `Authorization` header should record that lesson once and never re-discover it.

## How it surfaces

Three integrated mechanisms:

1. **The `/lesson` writ.** The agent records a lesson explicitly when it works out a remediation.
2. **Pre-turn injection.** Before each top-level turn, the runtime probes the agent's lessons against the user's prompt and prepends the top matches as a `KNOWN PITFALLS` block.
3. **Intra-turn loop detection.** When the same `(tool, args)` call fails twice in a row inside one turn, the runtime injects a `[LOOP DETECTED]` warning at the top of the next user-role tool-result block. Repetition is a strong signal the agent isn't reading its own results — calling it out usually breaks the loop.

## The `/lesson` writ

```
/lesson <signature>: <lesson text>
```

Single-line form. The signature is short — typically the tool or pattern that triggers the failure (`/fetch behind cloudflare`, `/exec rm -rf`, `JSON parsing in /read`). The lesson text is the remediation.

```
/lesson <signature>
<multi-line lesson body>
/endlesson
```

Block form for longer notes, terminated with `/endlesson`. The header line carries the signature; the body is the lesson.

```
/lesson list
/lesson search <query>
/lesson delete <id>
```

Subcommands. Listing surfaces the agent's lessons newest-consulted first (so frequently-applied lessons rise to the top). Search does substring match on signature + lesson_text. Delete removes a row.

A successful `/lesson` capture looks like:

```
[/lesson create /fetch behind cloudflare: use Authorization-bearing fetch, not plain GET]
OK: recorded lesson #14 — #14  [/fetch behind cloudflare]  use Authorization-bearing fetch, not plain GET
[END LESSON]
```

## Pre-turn injection

At the top of `Orchestrator::run_dispatch` (depth 0 only), the runtime calls the lesson invoker with `kind=preamble` and the user's prompt as the query string. The factory:

1. Extracts up to ~12 keyword tokens (lowercase, alpha-only, ≥4 chars).
2. Runs `search_lessons` for each keyword, deduping by id.
3. Ranks by `hit_count DESC, last_seen_at DESC`; takes top 3.
4. Bumps `hit_count` + `last_seen_at` on each surfaced lesson (so consulted lessons keep ranking up).
5. Returns a renderable block.

Result prepended to the user message:

```
[KNOWN PITFALLS — your prior lessons]
  - [/fetch behind cloudflare] use Authorization-bearing fetch, not plain GET (#14)
  - [JSON parsing in /read] /read returns base64 for binary; decode before json_parse (#22)
[END KNOWN PITFALLS]

<original user message>
```

Sub-agent depth ignores the probe — sub-agents already get pipeline memory + open todos in the `[DELEGATION CONTEXT]` block; piling pitfalls on top bloats the prompt without adding new signal. Sub-agents emit `/lesson list` explicitly when they want to consult their own.

The probe is best-effort: a missing tenant store, an empty result, or any thrown exception falls back to no injection rather than blocking dispatch.

## Intra-turn loop detection

The dispatch loop tracks which `(tool, args)` signatures produced an `ERR:` result in the previous iteration. When the same signature ERRs twice in a row, the runtime prepends:

```
[LOOP DETECTED]
The following tool calls have ERR'd twice in a row — repeating them won't change the result.
Change argument, change tool, ask for help, or stop trying:
  /fetch https://example.com/blocked
[END LOOP DETECTED]
```

…to the next iteration's user-role tool-result block. The agent sees the warning before its next reasoning pass; in practice the warning is what gets it to step out of the loop and try something else (or call `/lesson` to record what worked).

This is purely runtime — no persistence. It catches loops within a single turn-sequence; the persistent counterpart is the `/lesson` writ + pre-turn injection.

## Storage

One SQLite table, tenant-scoped with `ON DELETE CASCADE`:

| Column          | Notes                                                                       |
|-----------------|-----------------------------------------------------------------------------|
| `id`            | autoincrement PK                                                            |
| `tenant_id`     | FK to `tenants`                                                              |
| `agent_id`      | the calling agent's id at create time — lessons follow the agent identity   |
| `signature`     | short trigger pattern (≤ 200 chars)                                          |
| `lesson_text`   | remediation body (≤ 4096 chars)                                              |
| `hit_count`     | bumped each time the lesson is surfaced via the preamble probe              |
| `created_at` / `updated_at` / `last_seen_at` | epoch seconds                                          |

Indexed on `(tenant_id, agent_id, last_seen_at DESC)` for the at-a-glance list and `(tenant_id, agent_id, signature)` for the loop-detector lookup.

## Tenancy and trust posture

Every read and write enforces tenant_id match. Lessons never cross tenant or agent boundaries — a `research` agent's lessons aren't visible to a `writer` agent in the same tenant. This is a deliberate split: lessons capture an agent's individual relationship with its tools, and conflating different agents' lessons would dilute the signal.

## See also

- [`POST /v1/lessons`](../api/lessons/create.md), [`GET /v1/lessons`](../api/lessons/list.md), [`GET /v1/lessons/:id`](../api/lessons/get.md), [`PATCH /v1/lessons/:id`](../api/lessons/patch.md), [`DELETE /v1/lessons/:id`](../api/lessons/delete.md)
- [Writ DSL](writ.md) — the slash-command surface, including `/lesson`
- [Structured memory](structured-memory.md) — sister persistence layer for facts (vs. self-reflection)
- [Todos](todos.md) — the work tracker; lessons capture failures, todos capture work-in-progress
