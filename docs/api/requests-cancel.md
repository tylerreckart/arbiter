# `POST /v1/requests/:id/cancel`

**Auth:** tenant — _Status:_ stable

Cancel an in-flight orchestration. Wires to a Stop button in the UI. The `:id` is the `request_id` from the SSE `request_received` event of the call you want to stop.

## Request

### Path parameters

| Name | Type   | Description |
|------|--------|-------------|
| `id` | string | The `request_id` from the target request's SSE `request_received` event. |

No body, no query params.

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/requests/a889e53a7211eefa/cancel
```

## Response

### 200 OK — request found, cancellation issued

```json
{ "request_id": "a889e53a7211eefa", "cancelled": true }
```

The targeted stream still emits a final `done` event (typically with `ok: false`) so the SSE consumer sees a clean close.

### 404 Not Found — no in-flight request matches

The response is **deliberately the same** when:

- The request finished before the cancel arrived.
- The request never existed.
- The request belongs to a different tenant.

This avoids letting an attacker enumerate request ids across tenants.

```json
{
  "request_id": "a889e53a7211eefa",
  "cancelled": false,
  "reason": "no in-flight request with that id"
}
```

## Cancellation semantics

Cancellation is **best-effort**. A turn that's already returned from the LLM but hasn't been billed yet may complete; the next turn won't start. The cancelled stream still emits its final `done` event with whatever partial content was produced.

For multi-turn delegation: a cancel during a `/parallel` block waits for in-flight child threads to wind down (their LLM calls receive a cancel signal too), then the master receives an aggregated tool result and skips its next turn.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |
| 404    | Request not found, finished, or wrong tenant. | See above. |

## See also

- [`POST /v1/orchestrate`](orchestrate.md), [`POST /v1/agents/:id/chat`](agents/chat.md), [`POST /v1/conversations/:id/messages`](conversations/messages-post.md) — the streams this endpoint targets.
- [SSE event catalog](concepts/sse-events.md) — `request_received` and `done` shapes.
