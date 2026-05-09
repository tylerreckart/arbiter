# `DELETE /v1/todos/:id`

**Auth:** tenant — _Status:_ stable

Hard-delete a todo. Use [`PATCH`](patch.md) with `status: "canceled"` to keep the row in the archive instead.

## Request

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/todos/14
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
| 404    | Todo not found for this tenant. |

## See also

- [`PATCH /v1/todos/:id`](patch.md) — soft-archive via `status: "canceled"` instead.
- [Todos concept](../../concepts/todos.md)
