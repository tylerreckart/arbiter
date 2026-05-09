# `GET /v1/runs`

**Auth:** tenant — _Status:_ stable

Tenant-wide run history across every scheduled task. Newest `started_at` first; hard-capped at 200.

## Request

### Query parameters

| Param     | Description |
|-----------|-------------|
| `task_id` | Limit to one schedule's runs (equivalent to `/v1/schedules/:id/runs`). |
| `since`   | Epoch seconds. Returns runs with `started_at >= since`. Useful for incremental polling against the SSE notification stream. |

```bash
curl -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/runs?since=1746720000"
```

## Response

### 200 OK

```json
{
  "runs": [
    {
      "id": 42,
      "task_id": 17,
      "status": "succeeded",
      "started_at": 1746720000,
      "completed_at": 1746720185,
      "request_id": "a1b2c3d4e5f60718",
      "result_summary": "...",
      "input_tokens": 1240,
      "output_tokens": 187
    }
  ]
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |

## See also

- [`GET /v1/runs/:id`](get.md), [`GET /v1/schedules/:id/runs`](../schedules/runs.md)
- [`GET /v1/notifications/stream`](../notifications/stream.md) — push channel for new runs.
- [Scheduler concept](../../concepts/scheduler.md)
