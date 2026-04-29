# `GET /v1/artifacts`

**Auth:** tenant — _Status:_ stable

Tenant-wide cross-conversation artifact discovery. Useful for a sibling UI rendering "all my files" rather than "files in this thread". Same response shape as the conversation-scoped list, plus a `tenant_id` field.

## Request

No path / query / body params.

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/artifacts
```

## Response

### 200 OK

```json
{
  "tenant_id": 1,
  "count": 5,
  "artifacts": [ {/* ArtifactRecord, includes conversation_id */}, ... ],
  "bytes_used": 23148
}
```

Each `artifacts[]` entry includes its `conversation_id`. Field schemas: [Data model → ArtifactRecord](../concepts/data-model.md#artifactrecord).

Capped at 200 per page (no cursor in v1).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |

## See also

- [`GET /v1/artifacts/:aid`](get.md), [`GET /v1/artifacts/:aid/raw`](raw.md), [`DELETE /v1/artifacts/:aid`](delete.md).
- [`GET /v1/conversations/:id/artifacts`](conversations-list.md) — conversation-scoped variant.
