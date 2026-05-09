# `GET /v1/schedules`

**Auth:** tenant — _Status:_ stable

List the calling tenant's scheduled tasks. Newest `updated_at` first; hard-capped at 200.

## Request

### Query parameters

| Param    | Description |
|----------|-------------|
| `status` | Optional hard filter: `active` \| `paused` \| `completed` \| `failed`. Omit for all. |

```bash
curl -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/schedules?status=active"
```

## Response

### 200 OK

```json
{
  "schedules": [
    {
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
    }
  ]
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |

## See also

- [`POST /v1/schedules`](create.md), [`GET /v1/schedules/:id`](get.md), [`GET /v1/schedules/:id/runs`](runs.md)
- [Scheduler concept](../../concepts/scheduler.md)
