# `GET /v1/conversation-folders`

**Auth:** tenant — _Status:_ stable

List the tenant's conversation folders, ordered by `position` then name.

## Request

No query parameters.

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversation-folders
```

## Response

### 200 OK

```json
{
  "count": 1,
  "folders": [
    {
      "id": 3,
      "tenant_id": 1,
      "name": "Research",
      "position": 0,
      "created_at": 1777088000,
      "updated_at": 1777088000
    }
  ]
}
```

Field schemas: [Data model → ConversationFolder](../../concepts/data-model.md#conversationfolder).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`POST /v1/conversation-folders`](create.md), [`PATCH /v1/conversation-folders/:id`](patch.md), [`DELETE /v1/conversation-folders/:id`](delete.md).
- [`GET /v1/conversations?folder_id=`](../conversations/list.md).
