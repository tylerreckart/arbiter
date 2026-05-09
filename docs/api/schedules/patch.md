# `PATCH /v1/schedules/:id`

**Auth:** tenant — _Status:_ stable

Update a scheduled task's status. The single mutable field is `status` — other columns (phrase, message, agent) are immutable; recreate the schedule if you need to change them.

## Request

### Body

| Field    | Type   | Description |
|----------|--------|-------------|
| `status` | string | One of: `active`, `paused`, `canceled`, `completed`, `failed`. |

Resuming a recurring schedule whose `next_fire_at` is in the past advances it to the next valid fire time. Resuming a one-shot whose `fire_at` is in the past schedules it to fire on the next tick.

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"status":"paused"}' \
  http://arbiter.example.com/v1/schedules/17
```

## Response

### 200 OK

Returns the updated `scheduled_task`.

```json
{
  "scheduled_task": {
    "id": 17,
    "status": "paused",
    "...": "..."
  }
}
```

## Failure modes

| Status | When |
|--------|------|
| 400    | `status` value not in the allowed set; invalid JSON. |
| 401    | Missing / invalid bearer. |
| 404    | Schedule not found for this tenant. |

## See also

- [`GET /v1/schedules/:id`](get.md), [`DELETE /v1/schedules/:id`](delete.md)
- [Scheduler concept](../../concepts/scheduler.md)
