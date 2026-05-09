# `GET /v1/schedules/:id`

**Auth:** tenant — _Status:_ stable

Fetch one scheduled task by id. Tenant-scoped: a leaked id never surfaces another tenant's row.

## Request

```bash
curl -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/schedules/17
```

## Response

### 200 OK

Same `scheduled_task` shape as [`POST /v1/schedules`](create.md).

```json
{
  "scheduled_task": {
    "id": 17,
    "agent_id": "reviewer",
    "schedule_phrase": "every monday at 09:00",
    "schedule_kind": "recurring",
    "next_fire_at": 1746864000,
    "status": "active",
    "run_count": 4,
    "last_run_at": 1746720185,
    "last_run_id": 42,
    "...": "..."
  }
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Schedule not found for this tenant. |

## See also

- [`PATCH /v1/schedules/:id`](patch.md), [`DELETE /v1/schedules/:id`](delete.md), [`GET /v1/schedules/:id/runs`](runs.md)
- [Scheduler concept](../../concepts/scheduler.md)
