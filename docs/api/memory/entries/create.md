# `POST /v1/memory/entries`

**Auth:** tenant — _Status:_ stable

Create an entry in the structured memory graph. Entries created via this endpoint always land in `status="accepted"` (curated). Agent-originated entries go into the proposal queue with `status="proposed"` — see [Structured memory → Proposal queue](../../concepts/structured-memory.md#proposal-queue).

## Request

### Body

| Field         | Type          | Required | Constraint |
|---------------|---------------|----------|------------|
| `type`        | string (enum) | yes | One of `user`, `feedback`, `project`, `reference`, `learning`, `context`. |
| `title`       | string        | yes | 1–200 chars. |
| `content`     | string        | no  | ≤ 64 KiB. Defaults to `""`. |
| `source`      | string        | no  | Free-form provenance. ≤ 200 chars. |
| `tags`        | array<string> | no  | 0–32 tags, each 1–64 chars. Defaults to `[]`. |
| `artifact_id` | int           | no  | Optional FK to a [tenant artifact](../../concepts/artifacts.md). Validated against the tenant's catalogue — cross-tenant ids return 400. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"type":"reference","title":"Findings report","content":"Source: agent /write --persist","source":"agent","tags":["report"],"artifact_id":88}' \
  http://arbiter.example.com/v1/memory/entries
```

## Response

### 201 Created

The new `Entry`. Single-entry POST/GET/PATCH responses include a hydrated `artifact` block when the link is set; list and graph responses include only the bare `artifact_id` so paginated reads stay cheap.

```json
{
  "id": 42,
  "tenant_id": 1,
  "type": "reference",
  "title": "Findings report",
  "content": "Source: agent /write --persist",
  "source": "agent",
  "tags": ["report"],
  "status": "accepted",
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
  "created_at": 1777058449,
  "updated_at": 1777058449
}
```

Field schemas: [Data model → MemoryEntry](../../concepts/data-model.md#memoryentry).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; missing required field; `type` not in enum; `title` empty or > 200; `content` > 64 KiB; `tags` shape invalid; `artifact_id` doesn't exist for this tenant. | `{"error": "..."}` |
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`GET /v1/memory/entries`](list.md), [`GET /v1/memory/entries/:id`](get.md), [`PATCH /v1/memory/entries/:id`](patch.md), [`DELETE /v1/memory/entries/:id`](delete.md).
- [Structured memory](../../concepts/structured-memory.md), [Artifacts](../../concepts/artifacts.md).
