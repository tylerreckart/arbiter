# `GET /v1/conversations/:id`

**Auth:** tenant — _Status:_ stable

Fetch one conversation's metadata. Use [`GET /v1/conversations/:id/messages`](messages-list.md) for the message history.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversations/1
```

## Response

### 200 OK

The `Conversation` object. Field schemas: [Data model → Conversation](../concepts/data-model.md#conversation).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "conversation not found"}` |

## See also

- [`GET /v1/conversations/:id/messages`](messages-list.md), [`PATCH /v1/conversations/:id`](patch.md), [`DELETE /v1/conversations/:id`](delete.md).
