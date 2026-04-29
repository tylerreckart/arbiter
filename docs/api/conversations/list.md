# `GET /v1/conversations`

**Auth:** tenant — _Status:_ stable

List the tenant's conversations, newest-`updated_at` first. Drives the frontend's left-rail thread list.

## Request

### Query parameters

| Name | Type | Default | Description |
|------|------|---------|-------------|
| `before_updated_at` | int (epoch s) | none | Returns conversations updated strictly before this. Use the previous page's last `updated_at` to paginate backward. |
| `limit`             | int | 50 | Page size. Max 200. |

```bash
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/conversations?limit=20"
```

## Response

### 200 OK

```json
{
  "count": 1,
  "conversations": [
    {
      "id": 1,
      "tenant_id": 1,
      "title": "Q3 planning",
      "agent_id": "index",
      "created_at": 1777088000,
      "updated_at": 1777088752,
      "message_count": 4,
      "archived": false
    }
  ]
}
```

Field schemas: [Data model → Conversation](../concepts/data-model.md#conversation).

Archived rows are returned by default — clients filter for display. Use the `archived` flag on the conversation rows to hide them.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`POST /v1/conversations`](create.md), [`GET /v1/conversations/:id`](get.md), [`PATCH /v1/conversations/:id`](patch.md), [`DELETE /v1/conversations/:id`](delete.md).
