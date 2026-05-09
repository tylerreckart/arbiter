# Scheduler

A persistent background scheduler for agent work. Agents emit `/schedule "<phrase>": <message>` to defer or recur a task; the API server's tick thread fires due tasks through the same orchestrator path that `/v1/orchestrate` uses, persists the result as a run row, and publishes a notification on the SSE stream.

The scheduler runs only under `arbiter --api`. CLI / TUI / `--send` contexts do not have a long-running ticker; `/schedule` returns ERR there.

## Why it exists

Two patterns drove the design:

1. **Defer until conditions are met.** "Check the deploy in an hour and tell me if it stayed green." The agent doesn't want to block its current turn for an hour; it wants to queue the work and have the runtime call back.
2. **Recur cheaply.** "Every Monday at 9, summarise the week's PRs." Without a scheduler the only options are an external cron pinging `/v1/orchestrate`, or a long-lived loop the agent has to maintain manually.

Both are tenant-scoped, surface in the same `/v1/schedules` table, and produce a uniform run history per task.

## Lifecycle

| State        | Meaning                                                                 |
|--------------|--------------------------------------------------------------------------|
| `active`     | Will fire on or after `next_fire_at`. Default for new schedules.         |
| `paused`     | Won't fire until PATCHed back to `active`. Operator or agent toggle.     |
| `completed`  | Terminal for one-shot schedules after a successful fire.                 |
| `failed`     | Terminal for one-shot schedules whose fire raised — the run row carries the error message. |
| `canceled`   | Tombstone after explicit DELETE; rows are removed from the DB on cancel. |

A recurring task stays `active` indefinitely; each fire updates `next_fire_at`, increments `run_count`, and stamps `last_run_at` / `last_run_id`.

## Natural-language phrases

The parser recognises these forms (case-insensitive, whitespace-tolerant):

| Phrase                           | Kind       | Example                          |
|----------------------------------|------------|----------------------------------|
| `in N (minute\|hour\|day\|week)[s]` | one-shot   | `in 30 minutes`                  |
| `at HH:MM`                       | one-shot   | `at 17:00` (today; tomorrow if past) |
| `tomorrow [at HH:MM]`            | one-shot   | `tomorrow at 09:00`              |
| `on YYYY-MM-DD [at HH:MM]`       | one-shot   | `on 2026-06-01 at 12:00`         |
| `every hour` / `hourly`          | recurring  | `every hour`                     |
| `every N (minute\|hour)[s]`       | recurring  | `every 15 minutes`               |
| `every day [at HH:MM]` / `daily` | recurring  | `every day at 09:00`             |
| `every week [on <weekday>] [at HH:MM]` / `weekly` | recurring | `every week on monday at 09:00` |
| `every <weekday> [at HH:MM]`     | recurring  | `every monday at 09:00`          |

Times are local to the API server's host. Unparseable phrases return a tool-result `ERR:` block listing accepted forms.

## The `/schedule` writ

Agents emit:

```
/schedule in 1 hour: review the deploy status and report regressions
/schedule every monday at 09:00: summarise last week's merged PRs
/schedule list
/schedule cancel 42
/schedule pause 42
/schedule resume 42
```

The `:` separates the schedule phrase from the message that the agent will see when the task fires. The scheduled task targets the calling agent by default — a `research` agent emitting `/schedule …` queues work for `research`. Other agents are addressable via the HTTP `POST /v1/schedules` endpoint with an `agent` field.

A successful create returns:

```
[/schedule create in 1 hour: review the deploy ...]
OK: scheduled #17 — in 1 hour (2026-05-08 14:23) → research
  message: review the deploy status and report regressions
[END SCHEDULE]
```

## Fire path

When `next_fire_at <= now`:

1. Scheduler creates a `task_runs` row with `status='running'` and a fresh `request_id`.
2. Publishes a `run.started` notification.
3. Constructs an Orchestrator with the same wiring `/v1/orchestrate` uses (tenant memory, structured memory graph, MCP, A2A, search), runs `Orchestrator::send(agent_id, message)` synchronously.
4. Updates the run row with the final status (`succeeded` | `failed`), token counts, completed_at, and a truncated `result_summary`.
5. Updates the parent task: increments `run_count`, sets `last_run_at` / `last_run_id`, and either advances `next_fire_at` (recurring) or marks `status='completed'` (one-shot).
6. Publishes `run.completed` or `run.failed`.

A run that throws during orchestrator construction (missing tenant memory bridge, malformed config) marks the run failed but keeps recurring schedules active so transient init errors retry on the next tick. A missing target agent (`agent != "index"` and not in the catalog) pauses the schedule so the operator can fix the catalog before further fires.

## Notification stream

`GET /v1/notifications/stream` opens a long-lived SSE channel scoped to the caller's tenant. Each terminal run emits one `event: notification` block with the kind, task and run ids, status, agent, timestamps, and (on `run.completed`) a truncated `result_summary`:

```
event: open
data: {"ok":true}

event: notification
data: {"kind":"run.completed","task_id":17,"run_id":42,"agent_id":"research",
       "status":"succeeded","started_at":1746720000,"completed_at":1746720185,
       "result_summary":"…"}
```

A `: heartbeat` comment line every 30 seconds keeps reverse-proxy idle timeouts from collapsing the connection. Clients that miss events due to disconnection re-sync via `GET /v1/runs?since=<epoch>`; the SSE channel is convenience, not durable replay.

## Storage

Two SQLite tables, both tenant-scoped with `ON DELETE CASCADE`:

- `scheduled_tasks` — one row per schedule. Indexed on `(status, next_fire_at)` for the tick query and `(tenant_id, status, updated_at DESC)` for listing.
- `task_runs` — one row per fire. Indexed on `(task_id, started_at DESC)` and `(tenant_id, started_at DESC)`.

Both cascade-delete with the parent tenant. Hard-deleting a tenant erases all schedules and run history.

## Tenancy and trust posture

Every read and write enforces tenant_id match — there is no admin surface for cross-tenant schedule inspection. A leaked schedule id never surfaces another tenant's row.

Disabled tenants do not fire scheduled tasks. The scheduler tick checks `disabled` per task; recurring schedules push `next_fire_at` out an hour, one-shots transition to `paused`. Re-enabling the tenant resumes firing on the next tick.

## See also

- [`POST /v1/schedules`](../api/schedules/create.md), [`GET /v1/schedules`](../api/schedules/list.md), [`GET /v1/schedules/:id`](../api/schedules/get.md), [`PATCH /v1/schedules/:id`](../api/schedules/patch.md), [`DELETE /v1/schedules/:id`](../api/schedules/delete.md), [`GET /v1/schedules/:id/runs`](../api/schedules/runs.md)
- [`GET /v1/runs`](../api/runs/list.md), [`GET /v1/runs/:id`](../api/runs/get.md)
- [`GET /v1/notifications/stream`](../api/notifications/stream.md)
- [Writ DSL](writ.md) — the slash-command surface agents emit, including `/schedule`
- [`POST /v1/orchestrate`](../api/orchestrate.md) — the same fire path used for direct calls
