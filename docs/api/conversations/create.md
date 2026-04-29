# `POST /v1/conversations`

**Auth:** tenant — _Status:_ stable

Create a new conversation — a stored thread of messages between the tenant's user and one agent. The frontend uses these for the left-rail "previous chats" list, history reload, and persistent context across page navigations.

If you supply an inline `agent_def` here, the full definition is **snapshotted onto the conversation row** and reused on every subsequent [`POST /v1/conversations/:id/messages`](messages-post.md). The thread keeps working even if your sibling service later drops or changes the agent definition.

## Request

### Body

| Field       | Type   | Required | Default   | Description |
|-------------|--------|----------|-----------|-------------|
| `title`     | string | no  | `""`      | Display title. Empty until you set one (or auto-titling is added later). |
| `agent_id`  | string | no  | `"index"` | Which agent this conversation talks to. |
| `agent_def` | object | no  | —         | Inline agent definition (same shape as in chat requests). Required when `agent_id != "index"` if you want follow-ups to work without re-sending it on every turn — the definition is snapshotted onto the conversation row at create time. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"title":"Q3 planning","agent_id":"index"}' \
  http://arbiter.example.com/v1/conversations
```

## Response

### 201 Created

The new `Conversation` object — `id`, `title`, `agent_id`, `created_at`, `updated_at`, `message_count: 0`, `archived: false`, and `agent_def` if provided. Field schemas: see [Data model → Conversation](../concepts/data-model.md#conversation).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a JSON object; `agent_def` shape invalid. | `{"error": "..."}` |
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`GET /v1/conversations`](list.md), [`GET /v1/conversations/:id`](get.md), [`PATCH /v1/conversations/:id`](patch.md), [`DELETE /v1/conversations/:id`](delete.md).
- [`POST /v1/conversations/:id/messages`](messages-post.md) — the canonical multi-turn entrypoint.
