# `GET /v1/conversations/:id/artifacts`

**Auth:** tenant — _Status:_ stable

List the conversation's artifacts, newest-`updated_at` first. Capped at 200 per page (no cursor in v1; conversations of that size should migrate off the SQLite tier).

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversations/7/artifacts
```

## Response

### 200 OK

```json
{
  "conversation_id": 7,
  "count": 2,
  "artifacts": [ {/* ArtifactRecord */}, {/* ... */} ],
  "bytes_used": 1832,
  "tenant_bytes_used": 4231
}
```

Each `artifacts[]` entry is the metadata shape (no `content`). Field schemas: [Data model → ArtifactRecord](../concepts/data-model.md#artifactrecord).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Conversation not found / wrong tenant. | `{"error": "conversation not found"}` |

## See also

- [`POST /v1/conversations/:id/artifacts`](conversations-create.md), [`GET /v1/conversations/:id/artifacts/:aid`](conversations-get.md), [`GET /v1/conversations/:id/artifacts/:aid/raw`](conversations-raw.md), [`DELETE /v1/conversations/:id/artifacts/:aid`](conversations-delete.md).
- [`GET /v1/artifacts`](list.md) — tenant-wide cross-conversation discovery.
