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
| `graduated`         | bool-ish    | Only meaningful with `q` + `conversation_id`. Routes search through `search_entries_graduated`: runs both a conversation-scoped pass and a tenant-wide pass, then [reciprocal-rank-fuses](../../concepts/structured-memory.md#retrieval) the rankings (conversation pass weighted at 1.5×, tenant-wide at 1.0×). A strong tenant-wide hit can therefore outrank a weaker conversation-local one, fixing the multi-session case where the answer lives in another conversation. |
| `intent`            | string      | `intent=off` disables the heuristic question-intent classifier. Default behavior (and `intent=auto`) uses keyword cues in `q` ("favorite", "when", "how to", etc.) to soft-boost matching memory types via the existing 1.3× BM25 multiplier. Caller-supplied `type=…` always wins; intent boosts only apply when `type` is unset. Zero LLM cost. |
| `expand`            | string (model) | When set, the server calls this model once per request to generate 2 alternative phrasings of `q`, runs each through the full FTS pipeline (with its own intent classification), and reciprocal-rank-fuses the rankings (original at weight 1.0, paraphrases at 0.7). The no-embedding analogue of dense retrieval — closes the recall gap on paraphrased queries. Costs ~150 ms + ~$0.0001 per request. Failures (advisor unavailable, unparseable response) are benign: search proceeds with the original query alone and a `note` is surfaced in the `expansion` response block. |
| `rerank`            | string (model) | When set, takes the top-N FTS candidates and routes them through the model for a final reorder. The rerank prompt enriches each candidate row with `(type · YYYY-MM-DD · superseded)` metadata so the model can pick the most-recent non-superseded entry on temporal/knowledge-update questions. |
| `rerank_fine`       | string (model) | Two-stage rerank — pass 1 uses the cheaper `rerank` model on the wide pool, top 8 advance to pass 2 with this stronger model and bigger excerpts. Doubles LLM cost; typically lifts R@1. |
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

# Full ranking stack: graduated + query expansion + two-stage rerank
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/memory/entries?q=Postgres+performance+regression&conversation_id=17&graduated=true&expand=claude-haiku-4-5&rerank=claude-haiku-4-5&rerank_fine=claude-sonnet-4-6&limit=10"
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

When `expand` was passed, the response gains an `expansion` block:

```json
{
  "count": 10,
  "entries": [/* ... */],
  "expansion": {
    "model": "claude-haiku-4-5",
    "applied": true,
    "queries": [
      "Postgres slow query analysis",
      "database performance issue Postgres"
    ],
    "note": ""
  },
  "rerank": {
    "applied": true,
    "model": "claude-haiku-4-5",
    "fine_model": "claude-sonnet-4-6",
    "fine_applied": true,
    "stages": 2
  }
}
```

`expansion.applied` is `false` (with a populated `note`) when the advisor errored or returned no usable paraphrases — the search still ran on the original query.

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
