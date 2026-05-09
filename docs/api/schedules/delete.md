# `DELETE /v1/schedules/:id`

**Auth:** tenant — _Status:_ stable

Cancel and remove a scheduled task. Cascade: the FK on `task_runs` deletes every run row for the task as well. Use [`PATCH`](patch.md) with `status: "paused"` if you want to keep the run history.

## Request

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/schedules/17
```

## Response

### 200 OK

```json
{ "deleted": true }
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Schedule not found for this tenant. |

## See also

- [`PATCH /v1/schedules/:id`](patch.md) — pause without losing run history.
- [Scheduler concept](../../concepts/scheduler.md)
