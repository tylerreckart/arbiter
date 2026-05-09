# `PATCH /v1/todos/:id`

**Auth:** tenant — _Status:_ stable

Update one or more mutable fields. Any field omitted from the body is left untouched. Transitioning to a terminal `status` (`completed` or `canceled`) auto-stamps `completed_at = now()`.

## Request

### Body

| Field         | Type   | Description |
|---------------|--------|-------------|
| `subject`     | string | Rename the todo. |
| `description` | string | Replace the description. Pass `""` to clear. |
| `status`      | string | One of: `pending`, `in_progress`, `completed`, `canceled`. |
| `position`    | int    | Move within the pending bucket. |

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"status":"completed"}' \
  http://arbiter.example.com/v1/todos/14
```

## Response

### 200 OK

Returns the updated `todo`.

```json
{
  "todo": {
    "id": 14,
    "status": "completed",
    "completed_at": 1746720240,
    "...": "..."
  }
}
```

## Failure modes

| Status | When |
|--------|------|
| 400    | Invalid JSON; `status` not in the allowed set. |
| 401    | Missing / invalid bearer. |
| 404    | Todo not found for this tenant. |

## See also

- [`GET /v1/todos/:id`](get.md), [`DELETE /v1/todos/:id`](delete.md)
- [Todos concept](../../concepts/todos.md)
