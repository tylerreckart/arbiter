# `GET /v1/memory/proposals`

**Auth:** tenant — _Status:_ stable

List the agent-proposed entries and relations awaiting human review. The reviewer UI calls this to populate the accept/reject queue.

## Request

### Query parameters

| Name    | Type   | Description |
|---------|--------|-------------|
| `kind`  | string | `entries` or `relations` to scope to one collection. Default returns both. |
| `limit` | int    | Per-collection cap. Defaults: 100 entries, 200 relations. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/proposals
```

## Response

### 200 OK

```json
{
  "tenant_id": 1,
  "entries": [
    {
      "id": 88,
      "tenant_id": 1,
      "type": "project",
      "title": "Investigate cache stampede on /v1/orchestrate",
      "content": "",
      "source": "agent",
      "tags": [],
      "status": "proposed",
      "artifact_id": null,
      "created_at": 1777060001,
      "updated_at": 1777060001
    }
  ],
  "entries_count": 1,
  "relations": [
    {
      "id": 12,
      "tenant_id": 1,
      "source_id": 88,
      "target_id": 42,
      "relation": "relates_to",
      "status": "proposed",
      "created_at": 1777060002
    }
  ],
  "relations_count": 1
}
```

When `?kind=entries` is set, `relations`/`relations_count` are omitted; vice versa for `?kind=relations`.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | `kind` value not in `{"entries", "relations"}`. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |

## See also

- [`PATCH /v1/memory/entries/:id`](entries/patch.md), [`PATCH /v1/memory/relations/:id`](relations/patch.md) — accept proposals.
- [`DELETE /v1/memory/entries/:id`](entries/delete.md), [`DELETE /v1/memory/relations/:id`](relations/delete.md) — reject proposals.
- [Structured memory → Proposal queue](../concepts/structured-memory.md#proposal-queue).
