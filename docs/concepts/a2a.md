# Agent2Agent (A2A) protocol

Arbiter speaks the [Agent2Agent (A2A) protocol](https://a2a-protocol.org/) v1.0 in both directions.

- **Inbound**, every tenant agent is reachable as an A2A endpoint at `/v1/a2a/agents/:id`. Remote A2A clients — other agent frameworks, automation pipelines, multi-vendor orchestrators — call arbiter agents the same way they'd call any other A2A-compatible peer.
- **Outbound**, arbiter's own agents can delegate to remote A2A agents listed in `~/.arbiter/a2a_agents.json` via the [`/a2a` slash command](../../cli/a2a-agents.md). The master orchestrator sees them in its routing roster alongside local sub-agents and picks between them per turn.

A2A rides JSON-RPC 2.0 over HTTPS, with Server-Sent Events for streaming. Wire types are documented in the [official A2A specification](https://a2a-protocol.org/latest/specification/); arbiter implements a v1.0-only subset.

## When to reach for A2A vs `/v1/orchestrate`

| You want… | Use |
|-----------|-----|
| Drive arbiter from a JavaScript / Python / etc. arbiter-native client | [`POST /v1/orchestrate`](../orchestrate.md) — richer SSE event vocabulary, native to the runtime. |
| Plug arbiter into a multi-vendor agent fabric (anything else that speaks A2A) | [`POST /v1/a2a/agents/:id`](../a2a/dispatch.md) — spec-compatible JSON-RPC. |
| Persist a multi-turn thread on the server | `/v1/conversations/:id/messages` — A2A's `contextId` is opaque and not currently mapped to conversations. |

The two surfaces share the same per-request orchestrator setup, so an agent invoked through A2A has the same tool access (`/mem`, `/mcp`, `/search`, `/a2a`, sub-agent delegation) as one called via `/v1/orchestrate`.

## Endpoint surface

| Endpoint | Auth | What it does |
|----------|------|--------------|
| [`GET /.well-known/agent-card.json`](../a2a/well-known.md) | none | Top-level discovery stub. Tells unauth clients to authenticate and fetch a per-agent card. |
| [`GET /v1/a2a/agents/:id/agent-card.json`](../a2a/agent-card.md) | tenant | Per-agent A2A AgentCard derived from the agent's `Constitution`. |
| [`POST /v1/a2a/agents/:id`](../a2a/dispatch.md) | tenant | JSON-RPC 2.0 dispatch — all methods funnel here. |

## JSON-RPC method support

| Method | Status | Behaviour |
|--------|--------|-----------|
| `message/send` | implemented | Synchronous: blocks until terminal Task state, returns the assembled `Task`. |
| `message/stream` | implemented | Streams `TaskStatusUpdateEvent` and `TaskArtifactUpdateEvent` frames over SSE; final event has `final: true`. |
| `tasks/get` | implemented | Reads from the `a2a_tasks` table; tenant-scoped. |
| `tasks/cancel` | implemented | Cancels in-flight tasks via the existing `InFlightRegistry`; terminal tasks return `TaskNotCancelable`. |
| `tasks/resubscribe` | rejected | `UnsupportedOperation` (-32004). v1 doesn't store the per-event log needed to resubscribe. |
| `tasks/pushNotificationConfig/{set,get,list,delete}` | rejected | `UnsupportedOperation`. Push notifications are a v2 item. |

Unknown methods land at `MethodNotFound` (-32601). Malformed envelopes land at `ParseError` (-32700) or `InvalidRequest` (-32600). See the [`POST /v1/a2a/agents/:id`](../a2a/dispatch.md) endpoint page for the full error code table.

## Version negotiation

Arbiter speaks A2A v1.0 only.

- Send `A2A-Version: 1.0` (or `A2A-Version: 1`) on every request, or omit the header — both are accepted.
- Any other value triggers a `VersionNotSupportedError` (-32007).
- The well-known path is `/.well-known/agent-card.json`. The earlier v0.x path `/.well-known/agent.json` is **not** served — arbiter's stance is that v1.0 is stable enough to be the only conformance target.

## AgentCard derivation

Each A2A `AgentCard` is built from the agent's `Constitution` at fetch time:

| AgentCard field | Source |
|-----------------|--------|
| `protocolVersion` | Always `"1.0"`. |
| `name` | `Constitution.name` (falls back to the URL `:id`). |
| `description` | `Constitution.role + " — " + Constitution.goal`. |
| `url` | `<public_base_url>/v1/a2a/agents/<id>`. |
| `version` | `tenant_agents.updated_at` for stored agents; `"index"` for the master. |
| `capabilities` | `{streaming: true, pushNotifications: false, stateTransitionHistory: false}`. |
| `defaultInputModes` | `["text/plain"]`. |
| `defaultOutputModes` | `["text/plain", "application/json"]`. |
| `skills[]` | One synthetic `chat` skill, plus one `Skill` per `Constitution.capabilities` entry (e.g. `/fetch` → `fetch-url`, `/search` → `web-search`). |
| `securitySchemes` | Single `bearer` HTTP-auth scheme. |
| `security` | `[{ "bearer": [] }]`. |
| `preferredTransport` | `"JSONRPC"`. |

`public_base_url` is set via `ApiServerOptions::public_base_url` (use this when terminating TLS in a reverse proxy); without it, arbiter falls back to `http://<Host header>`.

## Task lifecycle states

Wire form is the lowercase hyphenated string from the v1.0 enum:

`submitted` → `working` → terminal (`completed` | `failed` | `canceled` | `rejected`)

Plus two interrupted states: `input-required` and `auth-required`. Arbiter's current handlers don't transition through `input-required` — a `message/send` either completes or fails outright. `auth-required` is reserved for future OAuth2-based remote-tool flows.

`completed`, `failed`, `canceled`, `rejected` are terminal. `tasks/cancel` against a terminal task returns `TaskNotCancelable` (-32002).

## Streaming event mapping

`message/stream` opens an SSE response. Every chunk is one JSON-RPC response envelope wrapping exactly one of `Task`, `Message`, `TaskStatusUpdateEvent`, or `TaskArtifactUpdateEvent`. Arbiter's internal events translate as follows:

| Arbiter event | A2A event | Notes |
|---------------|-----------|-------|
| `stream_start` (depth=0) | `TaskStatusUpdateEvent { state: working, final: false }` | Master agent starts. |
| `text` delta (depth=0) | `TaskArtifactUpdateEvent` (text part, `append: true`) | All chunks land on a single `<task_id>-text-0` artifact named `response`. |
| `tool_call` | `TaskArtifactUpdateEvent` (data part `{tool, ok}`) | One artifact per tool call. Metadata `x-arbiter.tool_call: true`. |
| `file` (`/write` capture) | `TaskArtifactUpdateEvent` (file part, inline bytes) | Filename in `artifact.name`. Bytes are utf-8 source code in v1; binary captures need base64 encoding (not yet wired). |
| `sub_agent_response` | `TaskArtifactUpdateEvent` (data part `{agent, depth, content}`) | Sub-agent text isn't streamed; it lands as one summary artifact per delegated turn. |
| `done(ok)` | `TaskStatusUpdateEvent { state: completed, final: true, message: <reply> }` | Final assistant message rides on the terminal status update. |
| `done(!ok)` / `error` | `TaskStatusUpdateEvent { state: failed, final: true }` | The error string lives on the persisted `a2a_tasks.error_message` column for `tasks/get`. |

A2A has no native token-delta primitive. Arbiter uses `append: true` text artifacts as the substitute; some clients won't render until they see `lastChunk: true`. We emit one final `lastChunk: true` text frame just before the terminal status update so spec-strict clients flush their accumulator.

Sub-agent text deltas (depth > 0) are **not** streamed — only the master agent's text feeds the primary artifact. Each sub-agent's full reply lands as one `data` artifact at end-of-turn via the `progress` callback. This avoids interleaving multiple agents' text into a single artifact.

## Authentication

Tenant bearer token. The same token used for `/v1/orchestrate` works for `/v1/a2a/agents/:id`. The agent card declares it via:

```json
{
  "securitySchemes": { "bearer": { "type": "http", "scheme": "bearer" } },
  "security": [ { "bearer": [] } ]
}
```

Multi-tenancy is per-agent: agents are tenant-scoped via the path `:id` resolving against `tenant_agents` for the bearer's tenant. A leaked token never surfaces another tenant's agent. The well-known stub at `/.well-known/agent-card.json` is unauth and intentionally minimal — it doesn't enumerate any agents; it just tells callers how to authenticate.

## Persistence

Tasks persist in a new `a2a_tasks` SQLite table:

```sql
CREATE TABLE a2a_tasks (
  task_id            TEXT    PRIMARY KEY,    -- == arbiter request_id
  tenant_id          INTEGER NOT NULL,
  agent_id           TEXT    NOT NULL,
  context_id         TEXT    NOT NULL DEFAULT '',
  state              TEXT    NOT NULL,       -- TaskState string
  created_at         INTEGER NOT NULL,
  updated_at         INTEGER NOT NULL,
  final_message_json TEXT    NOT NULL DEFAULT '',
  error_message      TEXT    NOT NULL DEFAULT ''
);
```

`task_id` reuses the arbiter `request_id` so [`/v1/requests/:id/cancel`](../requests-cancel.md) and `tasks/cancel` cancel the same in-flight orchestrator. Rows are written at three points: on submission (`submitted`), when streaming starts (`working`), and at terminal state (`completed | failed | canceled`).

`context_id` is opaque from arbiter's perspective in v1 — it threads through the protocol verbatim and is **not** foreign-keyed against `conversations`. A future revision may map A2A context to a conversation row so `/write --persist`, `/read`, and `/list` work in A2A-driven sessions; right now those callbacks are deliberately not wired. Files written via `/write` still flow as A2A `TaskArtifactUpdateEvent` frames in the streaming path.

## Tenancy and trust posture

When arbiter calls a remote A2A agent (outbound), it sends only the message text. The remote does not see:

- the calling tenant's bearer token,
- the structured-memory graph,
- local artifacts,
- other agents in the catalog.

The remote agent's reply flows back as a tool result; the calling agent decides what to persist via the existing `/mem*` and `/write` slash commands. The trust boundary is enforced by being on the remote side — there's no shared state to leak.

When a remote A2A client calls into arbiter (inbound), it gets exactly one tenant's slice of the world: the agents in that tenant's catalog, the master orchestrator, and the tenant-scoped tools (memory, MCP, search, sub-agent delegation, /a2a outbound). Every tool callback is rebound on each request — no cross-request state.

## Known gaps

| Gap | Tracked under |
|-----|---------------|
| No `tasks/resubscribe` | v2 — needs a per-task event log table to replay missed events. |
| No push notifications | v2 — the spec is still moving on auth handshake details. |
| No `auth-required` flow | future — applies once arbiter agents call OAuth2-protected remotes. |
| `contextId` ↛ conversation mapping | a follow-up will let `/write --persist`, `/read`, and `/list` work for A2A sessions. |
| Inline `agent_def` via A2A metadata | not in v1 — locked-in product decision. Agents must come from the URL or the tenant catalog. |
| Spec versions other than v1.0 | by design — any other version is rejected with `VersionNotSupportedError` (-32007). |

## See also

- [Per-agent card endpoint](../a2a/agent-card.md)
- [Well-known discovery stub](../a2a/well-known.md)
- [JSON-RPC dispatch endpoint](../a2a/dispatch.md)
- [`/a2a` slash command + registry](../../cli/a2a-agents.md)
- [`POST /v1/orchestrate`](../orchestrate.md) — the arbiter-native counterpart.
- [Authentication](authentication.md) — how the tenant bearer is validated.
- [SSE event catalog](sse-events.md) — the arbiter-native events that arbiter's own SSE dialect emits; A2A streaming uses different event shapes.
