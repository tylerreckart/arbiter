# `GET /v1/requests`

**Auth:** tenant — _Status:_ stable

List the calling tenant's recent orchestrate runs. Newest `started_at` first; capped at 200.

## Request

### Query parameters

| Param   | Description                                  |
|---------|----------------------------------------------|
| `limit` | Cap (default 100, max 200).                   |

```bash
curl -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/requests?limit=20"
```

## Response

### 200 OK

```json
{
  "requests": [
    {
      "request_id": "a1b2c3d4e5f60718",
      "agent_id": "research",
      "conversation_id": 7,
      "state": "completed",
      "started_at": 1746810000,
      "completed_at": 1746810185,
      "error_message": "",
      "last_seq": 142
    }
  ]
}
```

`state` is one of `running`, `completed`, `failed`, `canceled`.

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |

## See also

- [`GET /v1/requests/:id`](get.md), [`GET /v1/requests/:id/events`](events.md)
- [Durable in-flight execution](../../concepts/durable-execution.md)
