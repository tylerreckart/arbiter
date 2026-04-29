# `GET /v1/memory/graph`

**Auth:** tenant — _Status:_ stable

Single bulk fetch — entries plus relations in one round trip. The frontend calls this on mount to hydrate the force graph. **Curated graph only** — proposed entries and edges are hidden so the visualised graph never shows pending review items.

## Request

### Query parameters

| Name   | Type       | Description |
|--------|------------|-------------|
| `type` | csv string | Filter entries to these types; relations are pruned to those whose endpoints both survive the filter, so the snapshot stays self-consistent. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/graph
```

## Response

### 200 OK

```json
{
  "tenant_id": 1,
  "entries":   [ ... ],
  "relations": [ ... ]
}
```

`entries[]` and `relations[]` carry the standard shapes — see [Data model](../../concepts/data-model.md#memoryentry).

No pagination in v1 — the unfiltered set is expected to fit in one response. The entry sweep caps the snapshot at the per-page entry ceiling (200); when a tenant outgrows that we'll add cursor support.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | `type` value not in enum. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |

## See also

- [`GET /v1/memory/entries`](entries/list.md), [`GET /v1/memory/relations`](relations/list.md) — paginable per-resource lists.
- [`GET /v1/memory/proposals`](proposals.md) — proposal queue.
