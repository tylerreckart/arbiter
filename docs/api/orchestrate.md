# `POST /v1/orchestrate`

**Auth:** tenant — _Status:_ stable

Runs one agent request end-to-end and streams the result as Server-Sent Events. The full agentic loop happens inside this one call: master agent turns, delegated and parallel sub-agent calls, tool invocations (`/fetch`, `/search`, `/browse`, `/mcp`, `/a2a`, `/write`, `/mem*`, `/agent`, `/parallel`), generated files. The stream ends with a `done` event (success or controlled failure).

For a multi-turn thread that persists user / assistant messages on the server, use [`POST /v1/conversations/:id/messages`](conversations/messages-post.md) — same SSE shape, plus history replay and message persistence.

Spec-compatible Agent2Agent (A2A) clients can call the same agents via [`POST /v1/a2a/agents/:id`](a2a/dispatch.md), which translates between the arbiter event vocabulary and A2A `TaskStatusUpdateEvent` / `TaskArtifactUpdateEvent` frames. See the [A2A concept page](concepts/a2a.md) for the full mapping.

## Request

### Body

| Field        | Type     | Required | Default   | Description |
|--------------|----------|----------|-----------|-------------|
| `message`    | string   | yes      | —         | The prompt to send to the agent. |
| `agent`      | string   | no       | `"index"` | Which agent to address. Any stored agent id, the built-in `"index"` master, or (with `agent_def`) a caller-supplied UUID. |
| `agent_def`  | object   | no       | —         | Inline agent definition. See [Inline agents](#inline-agents) below. When set, overrides any stored agent at this id for this one request. |

### Headers

`Content-Type: application/json`. Tenant bearer token in `Authorization`.

### Inline agents

Send a complete agent configuration in the request body to run it for one call without persisting it. Useful when your sibling service stores agent definitions in its own database and treats arbiter as a stateless compute layer.

```json
{
  "message": "summarize this RFC",
  "agent_def": {
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "name": "Acme RFC Reviewer",
    "role": "rfc reviewer",
    "model": "claude-haiku-4-5",
    "goal": "extract decisions and tradeoffs from technical RFCs",
    "brevity": "bullets",
    "max_tokens": 512,
    "temperature": 0.2,
    "rules": [
      "lead with the decision being made",
      "flag any alternatives considered",
      "quote the rationale verbatim where relevant"
    ],
    "capabilities": ["research"],
    "advisor": {
      "model": "claude-opus-4-7",
      "mode": "consult"
    }
  }
}
```

`agent_def` field schema is in the [Agent data model](concepts/data-model.md#agent-catalog-row).

### ID resolution precedence

When multiple id sources are present, they must agree or the request fails with `400`:

1. `agent_def.id`
2. Path `:id` (for [`POST /v1/agents/:id/chat`](agents/chat.md)).
3. Body `agent` field.
4. Fallback: `"index"`.

The orchestrator preloads **every** stored agent for the tenant before the turn runs (so `/agent <stored_id>` and `/parallel`-fan-outs to stored siblings work without inline definitions). Inline `agent_def` still wins on a colliding id, allowing mid-thread overrides.

### Constraints

- Cannot override `"index"` — pick a different id. The master orchestrator is held as a separate runtime object.
- Lifetime is exactly one request. After the response completes, the orchestrator and its transient agents are destroyed. Only the agent's memory file (if `id` was set) survives.
- No agent-definition validation beyond `Constitution::from_json` — bad `model` strings, out-of-range `max_tokens`, etc., surface as upstream errors when the request is actually made.

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"agent":"index","message":"research gpt-5 vs claude-opus-4-7 for code review"}' \
  http://arbiter.example.com/v1/orchestrate
```

## Response

`Content-Type: text/event-stream`. `Connection: close`. One request per connection (no multiplexing).

The stream begins with `request_received`, contains a sequence of `stream_start` / `text` / `tool_call` / `file` / `token_usage` / `sub_agent_response` / `stream_end` events for the master and any delegated sub-agents, and ends with exactly one `done` event (or, on fatal error, an `error` event followed by `done` with `ok: false`).

`duration_ms` on `done` is wall-clock from request receipt to stream close. See the [SSE event catalog](concepts/sse-events.md) for full event-by-event field schemas, and [Fleet streaming](concepts/fleet-streaming.md) for the routing rules when `/parallel` is in play.

## Policy defaults

- **`/exec` disabled** — agents can't run shell commands on the server. An attempt returns an `ERR:` tool result so the agent adapts.
- **`/write` intercepted** — agent-generated files never hit the server's filesystem. They're streamed back as `file` events with UTF-8 content, subject to a 10 MiB per-response cap (configurable via `ApiServerOptions::file_max_bytes`). Persistent storage is opt-in via `/write --persist` — see [Artifacts](concepts/artifacts.md).
- **`/pane` unavailable** — pane spawning is a REPL-mode primitive; in API mode the master gets an `ERR:` and must use `/agent` (sequential) or `/parallel` (concurrent) instead.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a JSON object; missing `message`; `agent_def` shape invalid (Constitution parse fails); id-resolution conflict; attempt to override `"index"`. | `{"error": "..."}` |
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |
| 200 + `done.ok = false` | Errors that arise after the SSE stream opens (LLM upstream failure, cap exceeded, agent missing for non-`index` id with no inline / stored / snapshot fallback, transient I/O). The stream contains an `error` event followed by `done` with `ok: false`. The connection-level catch returns 200 because headers are already on the wire. | SSE stream |

The "after-headers" mode is intentional: by the time the SSE stream is open, returning a non-200 status code would split the response in confusing ways for clients. All recoverable errors come through as `error` SSE events with structured fields; the terminal `done.ok` flag is the canonical success/failure signal.

### Billing-service denial specifically

When the configured billing service rejects the pre-flight quota check (suspended tenant, exhausted budget, etc.):

1. `error` event with the upstream `message`, `reason` (`tenant_suspended` | `tenant_disabled` | `insufficient_budget`), and the matching `*_micro_cents` budget fields.
2. `done` with `ok: false` and no further turns.

A transport error to the billing service fails open — the runtime proceeds rather than blocking the request on a billing-service blip.

## See also

- [`POST /v1/agents/:id/chat`](agents/chat.md) — REST shape with a path-bound agent.
- [`POST /v1/conversations/:id/messages`](conversations/messages-post.md) — multi-turn variant with history replay.
- [`POST /v1/requests/:id/cancel`](requests-cancel.md) — interrupt an in-flight stream.
- [SSE event catalog](concepts/sse-events.md), [Fleet streaming](concepts/fleet-streaming.md).
