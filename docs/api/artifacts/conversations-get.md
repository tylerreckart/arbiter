# `GET /v1/conversations/:id/artifacts/:aid`

**Auth:** tenant — _Status:_ stable

Metadata for one artifact (no `content`). For the blob, use [`/raw`](conversations-raw.md).

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |
| `aid`      | int  | Artifact id. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversations/7/artifacts/12
```

## Response

### 200 OK

The `ArtifactRecord` shape:

```json
{
  "id": 12,
  "tenant_id": 1,
  "conversation_id": 7,
  "path": "output/report.md",
  "sha256": "ad14a...e3",
  "mime_type": "text/markdown",
  "size": 1832,
  "created_at": 1777060001,
  "updated_at": 1777060123
}
```

Field schemas: [Data model → ArtifactRecord](../concepts/data-model.md#artifactrecord).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist for this tenant + conversation pair. | `{"error": "artifact not found"}` |

## See also

- [`GET /v1/conversations/:id/artifacts/:aid/raw`](conversations-raw.md) — content blob.
- [`DELETE /v1/conversations/:id/artifacts/:aid`](conversations-delete.md).
