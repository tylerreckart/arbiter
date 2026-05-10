# `GET /v1/requests/:id`

**Auth:** tenant — _Status:_ stable

Fetch one orchestrate run's status. Tenant-scoped: a leaked id never surfaces another tenant's row.

## Request

```bash
curl -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/requests/a1b2c3d4e5f60718
```

## Response

### 200 OK

```json
{
  "request": {
    "request_id": "a1b2c3d4e5f60718",
    "agent_id": "research",
    "conversation_id": 7,
    "state": "running",
    "started_at": 1746810000,
    "completed_at": 0,
    "error_message": "",
    "last_seq": 38
  }
}
```

`last_seq` is useful as the `since_seq` argument to [`GET /v1/requests/:id/events`](events.md) when reconnecting.

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Request not found for this tenant. |

## See also

- [`GET /v1/requests`](list.md), [`GET /v1/requests/:id/events`](events.md)
- [`POST /v1/requests/:id/cancel`](../requests-cancel.md)
- [Durable in-flight execution](../../concepts/durable-execution.md)
