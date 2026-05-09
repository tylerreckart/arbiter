# `POST /v1/schedules`

**Auth:** tenant — _Status:_ stable

Create a scheduled task. The runtime parses the natural-language phrase, snapshots the resolved spec onto the row, and the scheduler tick thread fires it when `next_fire_at <= now`. Most agents create schedules via the [`/schedule` writ](../../concepts/writ.md); this HTTP shape is for front-ends that drive the surface directly.

## Request

### Body

| Field             | Type    | Required | Default   | Description |
|-------------------|---------|----------|-----------|-------------|
| `schedule`        | string  | yes      | —         | Natural-language phrase. See [Scheduler → Phrases](../../concepts/scheduler.md#natural-language-phrases) for accepted forms. |
| `message`         | string  | yes      | —         | The prompt the agent will receive when the task fires. |
| `agent`           | string  | no       | `"index"` | Target agent id. Must be `"index"` (the master) or an agent in the tenant catalog. |
| `conversation_id` | int     | no       | `0`       | Pin the task's runs to a conversation thread. `0` ⇒ unscoped. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"schedule":"every monday at 09:00","message":"summarise the week's PRs","agent":"reviewer"}' \
  http://arbiter.example.com/v1/schedules
```

## Response

### 201 Created

```json
{
  "scheduled_task": {
    "id": 17,
    "agent_id": "reviewer",
    "conversation_id": 0,
    "message": "summarise the week's PRs",
    "schedule_phrase": "every monday at 09:00",
    "schedule_kind": "recurring",
    "fire_at": 0,
    "recur_json": "{\"every\":\"week\",\"day\":\"mon\",\"at\":\"09:00\"}",
    "next_fire_at": 1746864000,
    "status": "active",
    "created_at": 1746720000,
    "updated_at": 1746720000,
    "last_run_at": 0,
    "last_run_id": 0,
    "run_count": 0
  },
  "normalized": "every Mon at 09:00"
}
```

`normalized` echoes the parser's canonical render, useful for confirmation UI ("scheduled: every Mon at 09:00").

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Missing `schedule` or `message`; phrase doesn't parse. | `{"error":"...", "accepted":"<help string>"}` on parse failure |
| 401    | Missing / invalid bearer. | `{"error":"..."}` |
| 404    | `agent` is not `"index"` and not in the tenant's catalog. | `{"error":"agent '...' not found"}` |

## See also

- [`GET /v1/schedules`](list.md), [`GET /v1/schedules/:id`](get.md), [`PATCH /v1/schedules/:id`](patch.md), [`DELETE /v1/schedules/:id`](delete.md), [`GET /v1/schedules/:id/runs`](runs.md)
- [Scheduler concept](../../concepts/scheduler.md)
