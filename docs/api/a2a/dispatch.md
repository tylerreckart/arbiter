# `POST /v1/a2a/agents/:id`

**Auth:** tenant — _Status:_ stable

JSON-RPC 2.0 dispatch endpoint for the [Agent2Agent (A2A) protocol](../concepts/a2a.md). All A2A methods funnel through this URL; the method is selected by the `method` field in the JSON-RPC envelope, not by the path.

Successful responses are returned as a single JSON-RPC envelope (HTTP 200 with `result`) for unary methods, or as Server-Sent Events for `message/stream`. Protocol errors round-trip as JSON-RPC error objects (HTTP 200 with `error.code`); only malformed envelopes that can't be answered in JSON-RPC form return a non-200 HTTP status.

`:id` follows the same resolution as [`GET /v1/a2a/agents/:id/agent-card.json`](agent-card.md) — the built-in `index` master plus any agent in the tenant's stored catalog.

## Request

### Headers

- `Content-Type: application/json`
- `Authorization: Bearer <tenant token>`
- `A2A-Version: 1.0` _(optional; `1` and empty also accepted; any other value triggers `VersionNotSupportedError`)_

### Body

JSON-RPC 2.0 envelope:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "message/send",
  "params": { "message": { ... } }
}
```

`id` may be string, number, or null; it's echoed verbatim on the response. `params` shape varies by method (below).

## Methods

### `message/send` — synchronous

Block until the agent reaches a terminal state, return the resulting `Task`.

**Params:**

```json
{
  "message": {
    "role": "user",
    "messageId": "m-1",
    "parts": [ { "kind": "text", "text": "summarize this RFC" } ],
    "contextId": "c-42"
  }
}
```

`messageId` is required. `contextId` is optional and threaded through verbatim — pass the same value across requests to group them logically. The context-thread relationship is not persisted server-side. Only `text` parts are accepted; `file` and `data` parts trigger `ContentTypeNotSupported` (-32005).

**Result:** a `Task` object.

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "kind": "task",
    "id": "f1a950de4f9b7d8d",
    "contextId": "c-42",
    "status": {
      "state": "completed",
      "message": { "role": "agent", "messageId": "f1a950de4f9b7d8d-r", "parts": [ { "kind": "text", "text": "..." } ], ... }
    },
    "history": [ /* user message + agent reply */ ],
    "metadata": {
      "x-arbiter.agent_id": "index",
      "x-arbiter.input_tokens": 142,
      "x-arbiter.output_tokens": 218
    }
  }
}
```

Token counts and the resolved `agent_id` ride on `metadata` under the `x-arbiter.*` prefix. Spec-aware clients ignore unknown metadata keys.

### `message/stream` — streaming

Same params as `message/send`. Response is `Content-Type: text/event-stream` with one SSE record per A2A event:

```
event: message
data: {"jsonrpc":"2.0","id":1,"result":{"kind":"status-update","taskId":"...","contextId":"...","status":{"state":"working"},"final":false}}

event: message
data: {"jsonrpc":"2.0","id":1,"result":{"kind":"artifact-update","taskId":"...","contextId":"...","artifact":{"artifactId":"...-text-0","name":"response","parts":[{"kind":"text","text":"hello "}]},"append":false}}

event: message
data: {"jsonrpc":"2.0","id":1,"result":{"kind":"artifact-update", ... ,"append":true}}

event: message
data: {"jsonrpc":"2.0","id":1,"result":{"kind":"status-update", ... ,"status":{"state":"completed","message":{...}},"final":true}}
```

Stream closes after the `final: true` `status-update`. See the [event mapping table](../concepts/a2a.md#streaming-event-mapping) for the full arbiter-event ↔ A2A-frame correspondence.

### `tasks/get`

**Params:** `{ "id": "<task_id>" }`

**Result:** a `Task` object reconstructed from the persisted `a2a_tasks` row. History is replayed only when a `final_message_json` snapshot exists (i.e., the task reached a terminal state). For tasks still working, `status.message` is omitted; the state alone is authoritative.

A task that doesn't exist for the calling tenant returns `TaskNotFound` (-32001). Cross-tenant lookups are blocked by the row's `tenant_id` filter.

### `tasks/cancel`

**Params:** `{ "id": "<task_id>" }`

**Result:** the canceled `Task`. Tries the in-flight registry first (same one [`POST /v1/requests/:id/cancel`](../requests-cancel.md) uses) and signals the orchestrator if the request is still running. Persists `state: canceled` regardless of the cancel-source race. Terminal tasks return `TaskNotCancelable` (-32002).

### `tasks/resubscribe`

**Params:** `{ "id": "<task_id>" }`

**Result:** `Content-Type: text/event-stream`. Replays the persisted event log for the task as A2A `TaskStatusUpdateEvent` / `TaskArtifactUpdateEvent` envelopes, then live-tails the run until terminal. Each arbiter event maps to the appropriate A2A frame; arbiter-specific event kinds (token usage, stream lifecycle) ride along under `x-arbiter.<kind>` metadata so spec-strict clients can ignore them.

If the task is already in a terminal state at fetch time, the handler emits one final `TaskStatusUpdateEvent` with `final: true` and closes. Backed by the same store as [`GET /v1/requests/:id/events`](../requests/events.md); see [Durable in-flight execution](../../concepts/durable-execution.md).

### Unsupported

`tasks/pushNotificationConfig/{set,get,list,delete}` return `UnsupportedOperation` (-32004) with a stable error message.

## Error codes

A2A-specific codes follow the v1.0 spec; standard JSON-RPC codes ride alongside.

| Code | Constant | When |
|------|----------|------|
| -32700 | `ParseError` | Body isn't valid JSON. |
| -32600 | `InvalidRequest` | Body parses but isn't a JSON-RPC request. |
| -32601 | `MethodNotFound` | `method` is not one of the supported names. |
| -32602 | `InvalidParams` | Required field missing on `params`, or shape mismatch. |
| -32603 | `InternalError` | Orchestrator init failure or other server-side fault. |
| -32001 | `TaskNotFound` | `tasks/get` / `tasks/cancel` against an unknown id, or `message/send` against an unknown agent for the tenant. |
| -32002 | `TaskNotCancelable` | `tasks/cancel` against a task in a terminal state. |
| -32004 | `UnsupportedOperation` | Method is not handled by arbiter (`tasks/pushNotificationConfig/*`). |
| -32005 | `ContentTypeNotSupported` | A `Part` had `kind != "text"`. |
| -32006 | `InvalidAgentResponse` | Agent threw mid-turn (provider transport failure surfaces as a failed `Task` instead — this is for unrecoverable execution faults). |
| -32007 | `VersionNotSupportedError` | `A2A-Version` header was set to anything other than `1.0` or `1`. |

A version-check failure happens before envelope parse, so the response carries `id: null` rather than echoing the request id.

## Cancellation

Two paths converge:

1. A2A `tasks/cancel` — JSON-RPC method on this endpoint.
2. Arbiter-native [`POST /v1/requests/:id/cancel`](../requests-cancel.md) — uses the same `request_id` (which equals the A2A `task_id`).

Either path signals the orchestrator. The streaming response emits a final `TaskStatusUpdateEvent` with `state: canceled, final: true` once the orchestrator unwinds.

## Failure modes (HTTP)

The endpoint emits a non-200 HTTP status only when no JSON-RPC envelope can be returned at all:

| HTTP | When |
|------|------|
| 401  | Missing or invalid bearer token (handled before this endpoint sees the request). |
| 404  | Bad URL shape (e.g. `/v1/a2a/foo`). |
| 405  | Wrong method (e.g. `GET` on the dispatch URL). |

Everything else lands at HTTP 200 with a JSON-RPC error object — this matches the spec, which keeps protocol-level errors inside the envelope so clients can branch on `error.code`.

## See also

- [A2A protocol concept](../concepts/a2a.md)
- [Per-agent card endpoint](agent-card.md)
- [Well-known discovery stub](well-known.md)
- [`POST /v1/orchestrate`](../orchestrate.md) — the arbiter-native counterpart with a richer event vocabulary.
- [`POST /v1/requests/:id/cancel`](../requests-cancel.md) — companion cancel surface.
