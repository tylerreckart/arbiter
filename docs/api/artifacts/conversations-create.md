# `POST /v1/conversations/:id/artifacts`

**Auth:** tenant — _Status:_ stable

Create or update an artifact in a conversation's working directory. Path validation, three-tier quotas, and PUT-on-conflict semantics all run here. See [Artifacts](../concepts/artifacts.md) for the full storage and safety model.

Same path validator as the agent's `/write --persist` slash command; HTTP and agent paths can't disagree on what's allowed.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id (must belong to caller's tenant). |

### Body

| Field       | Type   | Required | Description |
|-------------|--------|----------|-------------|
| `path`      | string | yes | Will be sanitized — caller may pass user-supplied paths. See [Artifacts → Path safety](../concepts/artifacts.md#path-safety) for rules. |
| `content`   | string | yes | UTF-8 string. Binary blobs over the JSON path is awkward; for binary content use a future direct-upload endpoint. |
| `mime_type` | string | no  | Defaults to `application/octet-stream`. Free-form; not sniffed. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"path":"output/report.md","content":"# Report\n...","mime_type":"text/markdown"}' \
  http://arbiter.example.com/v1/conversations/7/artifacts
```

## Response

### 201 Created — fresh insert
### 200 OK — overwrite (PUT-on-conflict)

```json
{
  "artifact": {
    "id": 12,
    "tenant_id": 1,
    "conversation_id": 7,
    "path": "output/report.md",
    "sha256": "ad14a...e3",
    "mime_type": "text/markdown",
    "size": 1832,
    "created_at": 1777060001,
    "updated_at": 1777060123
  },
  "tenant_used_bytes": 4231,
  "conversation_used_bytes": 1832,
  "created": false
}
```

`tenant_used_bytes` and `conversation_used_bytes` are post-write totals — clients can show a "you have used X of Y" hint.

PUT-on-conflict math: overwriting a 100 KB file with 200 KB only costs 100 KB against the conversation quota (the existing size is subtracted before the cap check).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; path fails sanitiser ([rules](../concepts/artifacts.md#path-safety)). | `{"error": "invalid path: <reason>"}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Conversation not found / wrong tenant. | `{"error": "conversation not found"}` |
| 409    | Race: path collision detected mid-INSERT (concurrent writer). Caller should retry as PUT. | `{"error": "path collision (concurrent write); retry"}` |
| 413    | Quota exceeded (per-file 1 MB / per-conversation 50 MB / per-tenant 500 MB). The body identifies which scope. | `{"error": "<scope> quota exhausted: ... > ... bytes"}` |

## See also

- [`GET /v1/conversations/:id/artifacts`](conversations-list.md), [`GET /v1/conversations/:id/artifacts/:aid`](conversations-get.md), [`GET /v1/conversations/:id/artifacts/:aid/raw`](conversations-raw.md), [`DELETE /v1/conversations/:id/artifacts/:aid`](conversations-delete.md).
- [Artifacts](../concepts/artifacts.md) — storage model, path safety, quotas, agent slash surface.
