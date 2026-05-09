# `GET /v1/schedules/:id/runs`

**Auth:** tenant — _Status:_ stable

Run history for one scheduled task. Newest `started_at` first; capped at 100. Use [`GET /v1/runs`](../runs/list.md) for tenant-wide cross-schedule listing.

## Request

```bash
curl -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/schedules/17/runs
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
      "result_summary": "Reviewed deploy 2f8e91. All green.",
      "error_message": "",
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
| 404    | Schedule not found for this tenant. |

## See also

- [`GET /v1/runs`](../runs/list.md), [`GET /v1/runs/:id`](../runs/get.md)
- [Scheduler concept](../../concepts/scheduler.md)
