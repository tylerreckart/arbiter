# `POST /v1/conversation-folders`

**Auth:** tenant — _Status:_ stable

Create a named conversation folder for the tenant.

## Request

### Body

| Field  | Type   | Required | Description |
|--------|--------|----------|-------------|
| `name` | string | yes      | Display name (non-empty). |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"name":"Research"}' \
  http://arbiter.example.com/v1/conversation-folders
```

## Response

### 201 Created

The new `ConversationFolder` object. Field schemas: [Data model → ConversationFolder](../../concepts/data-model.md#conversationfolder).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a JSON object; `name` missing/empty. | `{"error": "..."}` |
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`GET /v1/conversation-folders`](list.md), [`PATCH /v1/conversation-folders/:id`](patch.md), [`DELETE /v1/conversation-folders/:id`](delete.md).
