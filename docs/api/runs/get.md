# `GET /v1/runs/:id`

**Auth:** tenant — _Status:_ stable

Fetch one run row by id. Includes the full `result_summary` (truncated at 4 KiB at write time) and any `error_message` from a failed fire.

## Request

```bash
curl -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/runs/42
```

## Response

### 200 OK

```json
{
  "run": {
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
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Run not found for this tenant. |

## See also

- [`GET /v1/runs`](list.md), [`GET /v1/schedules/:id/runs`](../schedules/runs.md)
- [Scheduler concept](../../concepts/scheduler.md)
