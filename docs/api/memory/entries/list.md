# `GET /v1/memory/entries`

**Auth:** tenant — _Status:_ stable

List entries for the authenticated tenant. Two modes:

- **Browse** (no `q`): rows ordered by `updated_at DESC`. `type` and `tag` act as hard `WHERE` filters.
- **Search** (`q` present): rows ranked by Okapi-BM25 over an FTS5 index on `(title, content, tags, source)`. `type` and `tag` become **score boosts** (mismatched rows still appear, just lower in the order). See [Structured memory → Retrieval](../../concepts/structured-memory.md#retrieval) for the layered ranking pipeline.

Invalidated rows (those with a non-null `valid_to`) are excluded by default; pass `as_of=<epoch>` to read historical state.

## Request

### Query parameters

| Name                | Type        | Description |
|---------------------|-------------|-------------|
| `type`              | csv string  | `?type=project,reference` — OR-filter on the enum in browse mode; OR-boost in search mode. Unknown values reject 400. |
| `tag`               | string      | Single-tag substring match against the serialized JSON. Hard filter in browse mode; boost in search mode. |
| `q`                 | string      | FTS5 query. When set, switches to ranked-search mode; tokens are AND-combined with stemming and case-folding. |
| `conversation_id`   | int         | Scope results to one conversation. Returns rows pinned to this conversation **plus** unscoped rows (`conversation_id IS NULL`). Cross-tenant ids are silently dropped. |
| `graduated`         | bool-ish    | Only meaningful with `q` + `conversation_id`. Routes search through `search_entries_graduated`: conversation-scoped first, tenant-wide fill if the scoped pass returned fewer than `limit` hits. Conversation hits keep their order at the front. |
| `as_of`             | int (epoch) | Historical-window read. Returns rows whose validity window covers the given timestamp: `valid_from ≤ as_of AND (valid_to IS NULL OR valid_to > as_of)`. Default 0 = "active rows only." |
| `since`             | int (epoch) | `created_at >= since`. |
| `before_updated_at` | int (epoch) | `updated_at < before_updated_at`. Use for cursor pagination in browse mode. |
| `limit`             | int         | Default 50, hard max 200. |

```bash
# Browse: latest 50 references
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/memory/entries?type=reference&limit=50"

# Search: ranked, conversation-scoped graduated
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/memory/entries?q=deployment+notes&conversation_id=17&graduated=true"

# Audit: what was active at the start of last quarter
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/memory/entries?as_of=1735689600"
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
      "conversation_id": 17,
      "valid_from": 1777058449,
      "valid_to": null,
      "created_at": 1777058449,
      "updated_at": 1777058449
    }
  ]
}
```

`valid_to` is `null` for active rows, an epoch when invalidated. `conversation_id` is `null` when the entry is unscoped (visible from every conversation).

List responses include the bare `artifact_id` (no nested `artifact` block) to keep pagination cheap. Hit [`GET /v1/memory/entries/:id`](get.md) for the hydrated form.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Unknown `type` value. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |

## See also

- [`POST /v1/memory/entries`](create.md), [`GET /v1/memory/entries/:id`](get.md), [`PATCH /v1/memory/entries/:id`](patch.md), [`DELETE /v1/memory/entries/:id`](delete.md), [`POST /v1/memory/entries/:id/invalidate`](invalidate.md).
- [`GET /v1/memory/graph`](../graph.md) — bulk fetch including relations.
- [Structured memory → Retrieval](../../concepts/structured-memory.md#retrieval) — the ranking pipeline.
- [Structured memory → Temporal model](../../concepts/structured-memory.md#temporal-model) — `valid_from` / `valid_to` semantics.
