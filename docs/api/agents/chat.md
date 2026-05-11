# `POST /v1/agents/:id/chat`

**Auth:** tenant — _Status:_ stable

RESTful equivalent of [`POST /v1/orchestrate`](../orchestrate.md) with a path-bound agent id. Same SSE shape, same billing-service gating, same safety policies.

The path `:id` resolves through the same chain as `/v1/orchestrate` — inline `agent_def` first, then the tenant's stored catalog, then the built-in `index`. So you can hit a stored agent with no body beyond `message`.

## Request

| Path param | Type   | Description |
|------------|--------|-------------|
| `id`       | string | Agent id. Same resolution rules as `/v1/orchestrate`. |

### Body

| Field        | Type     | Required | Description |
|--------------|----------|----------|-------------|
| `message`    | string   | yes | The prompt. |
| `agent_def`  | object   | no  | Inline agent definition. If present, its `id` MUST match the path `:id`. |
| `agent`      | string   | —   | **Ignored** — the path wins. |

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"message":"write a haiku about SQLite"}' \
  http://arbiter.example.com/v1/agents/researcher/chat
```

### Headers

`Idempotency-Key` is honoured here with the same semantics as [`POST /v1/orchestrate`](../orchestrate.md#idempotency): a retry with the same key replays the original execution as SSE rather than triggering a second one.

With an inline `agent_def` (UUID-keyed memory persistence):

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{
    "message": "what did we decide about the migration last time?",
    "agent_def": {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "name": "Migration Reviewer",
      "role": "reviewer",
      "model": "claude-haiku-4-5",
      "goal": "review migrations and remember decisions"
    }
  }' \
  http://arbiter.example.com/v1/agents/550e8400-e29b-41d4-a716-446655440000/chat
```

## Response

Identical SSE stream to [`POST /v1/orchestrate`](../orchestrate.md). See the [SSE event catalog](../concepts/sse-events.md) for shapes.

## Failure modes

Identical to [`POST /v1/orchestrate`](../orchestrate.md), plus:

| Status | When | Body |
|--------|------|------|
| 400    | Body's `agent_def.id` ≠ path `:id`. | `{"error": "agent_def.id must match path :id"}` |

## See also

- [`POST /v1/orchestrate`](../orchestrate.md) — body-bound variant.
- [`POST /v1/conversations/:id/messages`](../conversations/messages-post.md) — multi-turn variant with history replay.
- [SSE event catalog](../concepts/sse-events.md).
