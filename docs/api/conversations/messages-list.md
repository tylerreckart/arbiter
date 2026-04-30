# `GET /v1/conversations/:id/messages`

**Auth:** tenant — _Status:_ stable

List messages in a conversation, oldest first (chat order, ready to render).

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |

### Query parameters

| Name       | Type | Default | Description |
|------------|------|---------|-------------|
| `after_id` | int  | 0       | Return messages with `id > after_id`. Useful for incremental polling. |
| `limit`    | int  | 200     | Page size. Max 500. |

```bash
curl -H "Authorization: Bearer atr_…" \
  "http://arbiter.example.com/v1/conversations/1/messages?after_id=10"
```

## Response

### 200 OK

```json
{
  "conversation_id": 1,
  "count": 4,
  "messages": [
    {
      "id": 1,
      "conversation_id": 1,
      "role": "user",
      "content": "what's a fanout in DSLs?",
      "input_tokens": 0,
      "output_tokens": 0,
      "created_at": 1777088746,
      "request_id": "a889e53a7211eefa"
    },
    {
      "id": 2,
      "conversation_id": 1,
      "role": "assistant",
      "content": "A fanout is …",
      "input_tokens": 1234,
      "output_tokens": 567,
      "created_at": 1777088752,
      "request_id": "a889e53a7211eefa"
    }
  ]
}
```

User messages have zero tokens (they're the input, not a measured turn). Assistant messages carry the **full request totals** (cumulative across the master + every delegated / parallel sub-agent for that turn). `request_id` correlates with the SSE stream and the matching billing-service usage record.

The assistant `content` field is the **cumulative** master output across every tool-call re-entry iteration, joined with newlines. This is what the next message replays into the agent's history. Field schemas: [Data model → ConversationMessage](../concepts/data-model.md#conversationmessage).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Conversation doesn't exist or belongs to another tenant. | `{"error": "conversation not found"}` |

## See also

- [`POST /v1/conversations/:id/messages`](messages-post.md) — append a turn.
