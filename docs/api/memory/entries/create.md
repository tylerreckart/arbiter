# `POST /v1/memory/entries`

**Auth:** tenant — _Status:_ stable

Create an entry in the structured memory graph. Both HTTP callers and agents write directly into the curated graph — see [Structured memory](../../concepts/structured-memory.md).

## Request

### Body

| Field             | Type          | Required | Constraint |
|-------------------|---------------|----------|------------|
| `type`            | string (enum) | yes | One of `user`, `feedback`, `project`, `reference`, `learning`, `context`. |
| `title`           | string        | yes | 1–200 chars. |
| `content`         | string        | no  | ≤ 64 KiB. Defaults to `""`. |
| `source`          | string        | no  | Free-form provenance. ≤ 200 chars. |
| `tags`            | array<string> | no  | 0–32 tags, each 1–64 chars. Defaults to `[]`. |
| `artifact_id`     | int           | no  | Optional FK to a [tenant artifact](../../concepts/artifacts.md). Validated against the tenant's catalogue — cross-tenant ids return 400. |
| `conversation_id` | int           | no  | Optional FK to a tenant conversation. Pins the entry to that conversation for [graduated search](../../concepts/structured-memory.md#retrieval). Validated against the tenant's catalogue. Omit (or pass 0) for an unscoped entry visible from every conversation. |
| `created_at`      | int (epoch)   | no  | Override the wall-clock timestamp. Accepts seconds (preferred) or milliseconds (auto-detected: values > 10¹² are treated as ms). Sets `created_at`, `updated_at`, and `valid_from` to the override value. Useful when backfilling historical transcripts so temporal queries see entries at their real authored time rather than ingest time. Default: server clock at insert. |
| `auto_tag`        | string (model) | no | When set, an advisor on this model extracts 2–4 lowercase hyphenated topical tags from `title` + `content` and merges them into `tags` before storage. Caller-provided `tags` win on conflicts. Tags carry an 8× weight in the BM25 ranking, so this is the cheapest way to boost retrieval signal on agent ingest paths that would otherwise leave tags empty. Failures (advisor unavailable, unparseable output) surface in the response's `auto_tag.note` and the entry is still created with the caller's tags. |
| `supersede`       | string (model) | no | When set, after the new entry is persisted an advisor on this model inspects the top-5 same-type FTS hits on the new title and decides whether the new entry directly contradicts any of them (e.g., new "I prefer pasta" supersedes prior "I prefer sushi"). Flagged ids get `valid_to=now()`. The prompt biases toward "leave alone" — false positives erase legitimate prior memory. Surfaces what was inspected and what was invalidated in the response's `supersede` block. |

`valid_from` is set automatically on insert (or overridden via `created_at`). `valid_to` starts NULL (active). Use [`POST /v1/memory/entries/:id/invalidate`](invalidate.md) to retire an entry; see [Structured memory → Temporal model](../../concepts/structured-memory.md#temporal-model).

Both `auto_tag` and `supersede` cost one advisor call each. They run independently; you can enable either, both, or neither. Per-agent defaults can be set via the `memory` block in [agent config](../../agents/create.md) so agents don't have to pass these parameters on every write.

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"type":"reference","title":"Findings report","content":"Source: agent /write --persist","source":"agent","tags":["report"],"artifact_id":88,"conversation_id":17}' \
  http://arbiter.example.com/v1/memory/entries
```

## Response

### 201 Created

The new `Entry`. Single-entry POST/GET/PATCH responses include a hydrated `artifact` block when the link is set; list and graph responses include only the bare `artifact_id` so paginated reads stay cheap.

When `auto_tag` was passed in the request, the response carries an `auto_tag` block with the model used, whether the run produced any tags, the tags that were merged in, and an optional note explaining a no-op (e.g., advisor unavailable). When `supersede` was passed, the response carries a `supersede` block with the candidates the advisor inspected and the ids that were invalidated.

```json
{
  "id": 42,
  "tenant_id": 1,
  "type": "reference",
  "title": "Findings report",
  "content": "Source: agent /write --persist",
  "source": "agent",
  "tags": ["report"],
  "artifact_id": 88,
  "artifact": {
    "id": 88,
    "conversation_id": 7,
    "path": "output/report.md",
    "sha256": "ad14a...",
    "mime_type": "text/markdown",
    "size": 1832,
    "created_at": 1777060001,
    "updated_at": 1777060001
  },
  "conversation_id": 17,
  "valid_from": 1777058449,
  "valid_to": null,
  "created_at": 1777058449,
  "updated_at": 1777058449
}
```

With `auto_tag` and `supersede` enabled, the response gains two trailing blocks:

```json
{
  "id": 42,
  "...": "...",
  "tags": ["report", "honeycomb", "pricing", "trace-first"],
  "auto_tag": {
    "model": "claude-haiku-4-5",
    "applied": true,
    "added": ["honeycomb", "pricing", "trace-first"],
    "note": ""
  },
  "supersede": {
    "model": "claude-haiku-4-5",
    "applied": true,
    "candidates": [38, 35, 22],
    "invalidated": [38],
    "note": ""
  }
}
```

`auto_tag.applied` is `true` when at least one tag was added, `false` on advisor failure or empty output (with `note` populated). `supersede.candidates` shows the full top-5 set the advisor was shown; `supersede.invalidated` is the subset that the advisor flagged and the server then stamped with `valid_to=now()`.

Field schemas: [Data model → MemoryEntry](../../concepts/data-model.md#memoryentry).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; missing required field; `type` not in enum; `title` empty or > 200; `content` > 64 KiB; `tags` shape invalid; `artifact_id` or `conversation_id` doesn't exist for this tenant. | `{"error": "..."}` |
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`GET /v1/memory/entries`](list.md), [`GET /v1/memory/entries/:id`](get.md), [`PATCH /v1/memory/entries/:id`](patch.md), [`DELETE /v1/memory/entries/:id`](delete.md), [`POST /v1/memory/entries/:id/invalidate`](invalidate.md).
- [Structured memory](../../concepts/structured-memory.md), [Artifacts](../../concepts/artifacts.md).
