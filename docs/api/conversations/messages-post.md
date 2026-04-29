# `POST /v1/conversations/:id/messages`

**Auth:** tenant — _Status:_ stable

Send a user message and stream the assistant's reply. Same SSE response shape as [`POST /v1/orchestrate`](../orchestrate.md), plus a `conversation_id` field on the `done` event. **Billed.**

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |

### Body

| Field       | Type   | Required | Description |
|-------------|--------|----------|-------------|
| `message`   | string | yes | The new user turn. |
| `agent_def` | object | no  | Override the conversation's snapshotted agent for this one turn (rare — usually a follow-up should just send `message`). When omitted, the conversation's snapshot from create time is reused. |

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"message":"and what about cache writes?"}' \
  http://arbiter.example.com/v1/conversations/1/messages
```

## What happens server-side

1. Conversation lookup — `404` if missing or wrong tenant. Validation surfaces as a clean JSON error before the SSE stream opens.
2. The conversation's snapshotted `agent_def` is applied to the orchestrator (unless the request body supplied its own, which wins). This is what makes follow-ups work without re-sending the agent definition.
3. Prior messages loaded from the DB and replayed into the agent's history (capped at the most recent 100 turns to keep request payload bounded).
4. The user's `message` is persisted with the `request_id` issued for this stream.
5. The orchestrator runs and streams events exactly as [`POST /v1/orchestrate`](../orchestrate.md) would.
6. On a successful `done`, the assistant's full cumulative response (across every tool-call re-entry iteration) is persisted alongside the request's billing totals (`input_tokens`, `output_tokens`, `billed_micro_cents`).
7. On failure (`done.ok = false`), the assistant message is **not** persisted — only the user message remains. Retry is safe.

## Response

`Content-Type: text/event-stream`. Identical to [`POST /v1/orchestrate`](../orchestrate.md), plus `conversation_id` on the terminal `done` event.

The `request_id` from this call's `request_received` event is the handle to pass to [`POST /v1/requests/:id/cancel`](../requests-cancel.md) if the user clicks Stop.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a JSON object; missing `message`; `agent_def` shape invalid. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Conversation doesn't exist or belongs to another tenant. | `{"error": "conversation not found"}` |
| 200 + `done.ok = false` | LLM upstream failure, cap exceeded, transient I/O. See [`POST /v1/orchestrate`](../orchestrate.md#failure-modes). | SSE stream |

## See also

- [`POST /v1/orchestrate`](../orchestrate.md) — single-turn variant without history replay.
- [`GET /v1/conversations/:id/messages`](messages-list.md) — read history back.
- [`POST /v1/requests/:id/cancel`](../requests-cancel.md), [SSE event catalog](../concepts/sse-events.md), [Billing](../concepts/billing.md).
