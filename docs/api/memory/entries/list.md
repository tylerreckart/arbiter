# `GET /v1/memory/entries`

**Auth:** tenant — _Status:_ stable

List entries for the authenticated tenant, newest `updated_at` first.

## Request

### Query parameters

| Name                | Type        | Description |
|---------------------|-------------|-------------|
| `type`              | csv string  | `?type=project,reference` — OR-filter on the enum. Unknown values reject 400. |
| `tag`               | string      | Single-tag substring match against the serialized JSON. |
| `q`                 | string      | LIKE-match on `title` + `content`. Case follows SQLite's default (case-insensitive ASCII). |
| `since`             | int (epoch) | `created_at >= since`. |
| `before_updated_at` | int (epoch) | `updated_at < before_updated_at`. Use for cursor pagination. |
| `limit`             | int         | Default 50, hard max 200. |

```bash
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/memory/entries?type=reference&limit=50"
```

## Response

### 200 OK

```json
{
  "count": 1,
  "entries": [
    {
      "id": 42,
      "tenant_id": 1,
      "type": "reference",
      "title": "Findings report",
      "content": "...",
      "source": "agent",
      "tags": ["report"],
      "artifact_id": 88,
      "created_at": 1777058449,
      "updated_at": 1777058449
    }
  ]
}
```

List responses include the bare `artifact_id` (no nested `artifact` block) to keep pagination cheap. Hit [`GET /v1/memory/entries/:id`](get.md) for the hydrated form.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Unknown `type` value. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |

## See also

- [`POST /v1/memory/entries`](create.md), [`GET /v1/memory/entries/:id`](get.md), [`PATCH /v1/memory/entries/:id`](patch.md), [`DELETE /v1/memory/entries/:id`](delete.md).
- [`GET /v1/memory/graph`](../graph.md) — bulk fetch including relations.
