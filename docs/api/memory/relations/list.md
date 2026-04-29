# `GET /v1/memory/relations`

**Auth:** tenant — _Status:_ stable

List relations for the authenticated tenant. **Curated graph only** — proposed edges (`status="proposed"`) are hidden; fetch them via [`GET /v1/memory/proposals`](../proposals.md).

## Request

### Query parameters

| Name        | Type   | Description |
|-------------|--------|-------------|
| `source_id` | int    | Filter to edges from this entry. |
| `target_id` | int    | Filter to edges to this entry. |
| `relation`  | string | Filter to one relation kind. |
| `limit`     | int    | Default 200, hard max 1000. |

```bash
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/memory/relations?source_id=42"
```

## Response

### 200 OK

```json
{
  "count": 1,
  "relations": [
    {
      "id": 7,
      "tenant_id": 1,
      "source_id": 42,
      "target_id": 43,
      "relation": "supports",
      "status": "accepted",
      "created_at": 1777058500
    }
  ]
}
```

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | `relation` not in enum. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |

## See also

- [`POST /v1/memory/relations`](create.md), [`PATCH /v1/memory/relations/:id`](patch.md), [`DELETE /v1/memory/relations/:id`](delete.md).
- [`GET /v1/memory/graph`](../graph.md) — bulk fetch.
