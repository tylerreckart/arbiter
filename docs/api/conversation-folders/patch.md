# `PATCH /v1/conversation-folders/:id`

**Auth:** tenant — _Status:_ stable

Rename a folder and/or change its sort position. Fields optional — apply whichever are present.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Folder id. |

### Body

| Field      | Type   | Description |
|------------|--------|-------------|
| `name`     | string | New display name. |
| `position` | int    | New sort order (≥ 0). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"name":"Labs","position":1}' \
  http://arbiter.example.com/v1/conversation-folders/3
```

## Response

### 200 OK

The updated `ConversationFolder` object. Field schemas: [Data model → ConversationFolder](../../concepts/data-model.md#conversationfolder).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a JSON object; `position` &lt; 0. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "folder not found"}` |

## See also

- [`GET /v1/conversation-folders`](list.md), [`POST /v1/conversation-folders`](create.md), [`DELETE /v1/conversation-folders/:id`](delete.md).
