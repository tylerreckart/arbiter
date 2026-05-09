# Todos

A persistent work tracker. Agents emit `/todo add …`, `/todo start <id>`, `/todo done <id>` to capture and mark progress on the steps they're working through; the tenant's todo store outlives any single conversation, so a `/schedule`-fired run can pick up the same todos a human-driven turn left behind.

The two natural homes for a todo:

- **Conversation-scoped** (the default when a conversation is bound to the request). The todo lives with the thread; a tenant browsing the conversation sees the same list the agent did.
- **Tenant-wide** (`conversation_id = 0`). Visible from every conversation, but also surfaced inside one. Useful for cross-thread initiatives the agent should remember between sessions.

A conversation-scoped read sees both — pinned rows plus the unscoped pool — same OR-NULL fallback the [structured memory](structured-memory.md) layer uses.

## Lifecycle

| Status         | Meaning                                                                |
|----------------|------------------------------------------------------------------------|
| `pending`      | Captured but not started. Default at create time.                      |
| `in_progress`  | The agent is actively working on it. Floats to the top of `/todo list`. |
| `completed`    | Done. Stamps `completed_at` and disappears from the active list.       |
| `canceled`     | Won't be done. Same archive treatment as `completed`.                  |

Status is the only column that auto-stamps anything else: any transition into `completed` or `canceled` sets `completed_at = now()` unless the caller passes a value explicitly. Pending → in_progress → completed is the canonical flow; you can also skip straight from pending to canceled when scope changes mid-task.

## The `/todo` writ

Agents emit slash commands; the dispatcher wraps results in `[/todo …]` / `[END TODO]` framing.

```
/todo add review the deploy status
/todo add write the post-mortem
<one or more lines of detail>
/endtodo
/todo list
/todo start 14
/todo done 14
/todo cancel 15
/todo describe 14: bumped to follow up next sprint
/todo subject 14: review the canary deploy status
```

`/todo add` accepts both the single-line form (subject only) and a block form terminated with `/endtodo` for multi-line descriptions. The block form is required when the description spans multiple lines because the runtime needs the explicit terminator to commit the row — without it, the writ is marked truncated and the agent gets an `ERR: missing /endtodo terminator` so it can retry rather than committing a half-written body.

A successful `/todo list` looks like:

```
[/todo list]
3 open (1 in progress, 2 pending):
▶ #14  [in_progress]  review the canary deploy status
  #15  [pending]      write the post-mortem
  #16  [pending]      file the rollback ticket  (tenant-wide)
[END TODO]
```

The `▶` marker singles out `in_progress` rows. A `(tenant-wide)` suffix means the row's `conversation_id` is 0 — it'll surface in this conversation but also in every other thread for the tenant.

## Pipeline-memory injection

When the master agent delegates to a sub-agent (via `/agent` or `/parallel`), the runtime probes the todo store for the calling conversation's open todos and prepends an `Open todos` block to the sub-agent's `[DELEGATION CONTEXT]` envelope:

```
[DELEGATION CONTEXT]
Original request: …
Delegated by: index
Pipeline depth: 1/2
Pipeline memory (entries written by prior agents this conversation — …):
  - #281  [project]  Q3 observability rollout — initial brief
Open todos (mark progress as you go — /todo start <id>, /todo done <id>):
3 open (1 in progress, 2 pending):
▶ #14  [in_progress]  review the canary deploy status
  #15  [pending]      write the post-mortem
  #16  [pending]      file the rollback ticket
[END DELEGATION CONTEXT]
```

This is symmetric with structured memory's pipeline injection: the sub-agent walks in already knowing what the caller was working on, so it can mark items done as it finishes them rather than re-discovering the list. The probe is best-effort — an empty list or any failure surface degrades silently rather than blocking delegation.

## Storage

One SQLite table, tenant-scoped with `ON DELETE CASCADE`:

| Column            | Notes                                                                |
|-------------------|----------------------------------------------------------------------|
| `id`              | autoincrement primary key                                            |
| `tenant_id`       | FK to `tenants`; cascade-deletes with the tenant                      |
| `conversation_id` | 0 = unscoped (visible everywhere); positive = pinned to that thread  |
| `agent_id`        | the calling agent's id at create time                                |
| `subject`         | required, short title                                                |
| `description`     | optional; multi-line text                                            |
| `status`          | `pending` / `in_progress` / `completed` / `canceled`                  |
| `position`        | append-order counter within `(tenant_id, conversation_id, status='pending')` so `/todo list` is stable on ties |
| `created_at` / `updated_at` / `completed_at` | epoch seconds; `completed_at = 0` until terminal |

Indexed on `(tenant_id, conversation_id, status, position)` for the per-thread list and `(tenant_id, status, updated_at DESC)` for tenant-wide browses.

## Tenancy and trust posture

Every read and write enforces tenant_id match — there is no admin surface for cross-tenant todo inspection. A leaked todo id never surfaces another tenant's row, and cross-tenant patches/deletes return as if the row didn't exist (the same 404 pattern conversations and structured memory use).

## See also

- [`POST /v1/todos`](../api/todos/create.md), [`GET /v1/todos`](../api/todos/list.md), [`GET /v1/todos/:id`](../api/todos/get.md), [`PATCH /v1/todos/:id`](../api/todos/patch.md), [`DELETE /v1/todos/:id`](../api/todos/delete.md)
- [Writ DSL](writ.md) — the slash-command surface, including `/todo`
- [Structured memory](structured-memory.md) — the sister persistence layer for facts (vs. work items)
- [Scheduler](scheduler.md) — pairs naturally with todos: a scheduled run that fires `/todo done <id>` as it finishes each step gives the human a passive progress feed.
