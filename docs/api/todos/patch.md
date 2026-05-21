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

## Batch form

For multi-row updates (mark several done, reorder a column, sync state from an
external tracker), use the batch endpoint instead of N HTTP round-trips:

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '[
        {"id":14,"status":"completed"},
        {"id":15,"status":"completed"},
        {"id":16,"position":1}
      ]' \
  http://arbiter.example.com/v1/todos
```

The body is either a JSON array of `{id, ...fields}` objects, or
`{"todos":[...]}` for forward-compatibility with future top-level options. Each
row is applied independently; per-row failures (`404`, invalid `status`) are
reported in the response without short-circuiting the rest:

```json
{
  "ok": 2,
  "errors": 1,
  "results": [
    {"id":14,"ok":true,"todo":{...}},
    {"id":15,"ok":true,"todo":{...}},
    {"id":99,"ok":false,"error":"todo not found"}
  ]
}
```

Cap: 500 items per batch (mirrors the `GET /v1/todos` list cap). The HTTP
status is always `200` when the body parses; per-row results carry the
fine-grained errors.

## See also

- [`GET /v1/todos/:id`](get.md), [`DELETE /v1/todos/:id`](delete.md)
- [Todos concept](../../concepts/todos.md)
