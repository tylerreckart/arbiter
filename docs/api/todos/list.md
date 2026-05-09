# `GET /v1/todos`

**Auth:** tenant — _Status:_ stable

List the calling tenant's todos. Ordered with `in_progress` first, then `pending` by ascending `position`, then terminal rows (`completed`, `canceled`) by most-recently updated. Hard-capped at 200; default limit is 200.

## Request

### Query parameters

| Param             | Description |
|-------------------|-------------|
| `conversation_id` | Positive integer scopes the list to one thread. The result includes both rows pinned to that conversation **and** unscoped (`conversation_id = 0`) rows — same OR-NULL fallback structured memory uses, so a fresh thread still surfaces tenant-wide todos. Omit for tenant-wide listing of every row. |
| `status`          | Hard filter: `pending` \| `in_progress` \| `completed` \| `canceled`. Omit for all statuses. |
| `agent_id`        | Hard filter on the owner-of-record (the agent that created the row). |
| `limit`           | Cap; max 500. |

```bash
curl -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/todos?conversation_id=7&status=pending"
```

## Response

### 200 OK

```json
{
  "todos": [
    {
      "id": 14,
      "conversation_id": 7,
      "agent_id": "index",
      "subject": "review the deploy",
      "description": "check logs + metrics",
      "status": "in_progress",
      "position": 1,
      "created_at": 1746720000,
      "updated_at": 1746720085,
      "completed_at": 0
    }
  ]
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |

## See also

- [`POST /v1/todos`](create.md), [`GET /v1/todos/:id`](get.md), [`PATCH /v1/todos/:id`](patch.md)
- [Todos concept](../../concepts/todos.md)
