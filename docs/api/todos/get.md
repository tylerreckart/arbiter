# `GET /v1/todos/:id`

**Auth:** tenant — _Status:_ stable

Fetch one todo by id. Tenant-scoped: a leaked id never surfaces another tenant's row.

## Request

```bash
curl -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/todos/14
```

## Response

### 200 OK

```json
{
  "todo": {
    "id": 14,
    "conversation_id": 7,
    "agent_id": "index",
    "subject": "review the deploy",
    "description": "check logs + metrics",
    "status": "completed",
    "position": 1,
    "created_at": 1746720000,
    "updated_at": 1746720240,
    "completed_at": 1746720240
  }
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Todo not found for this tenant. |

## See also

- [`PATCH /v1/todos/:id`](patch.md), [`DELETE /v1/todos/:id`](delete.md), [`GET /v1/todos`](list.md)
- [Todos concept](../../concepts/todos.md)
