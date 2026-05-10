# Durable in-flight execution

Every `/v1/orchestrate` call (and conversation message, agent chat, A2A dispatch) writes its SSE event stream to a durable log as it runs. A client whose connection drops mid-stream reconnects to `GET /v1/requests/:id/events?since_seq=N` and gets the rest of the run — replayed backlog plus a live tail until the run finishes. A2A clients use the equivalent `tasks/resubscribe` JSON-RPC call.

## Why

Without persistence:

- A network blip or browser refresh during a multi-minute agent turn loses everything the user already saw, plus everything that was emitted while they were offline. The LLM call already happened; the tokens were paid for; the result is gone.
- Cancelling and restarting "from where it left off" doesn't exist — there's no left-off state to resume from.
- Operator restart of the API server (deploy, OOM, signal) kills every in-flight stream with no remediation.

The durable log fixes all three: every event lands in SQLite as it's emitted, reconnects replay the missed prefix, and a recovery sweep at startup gives clients a clean terminal signal for runs the previous process didn't finish.

## Storage

Two SQLite tables on `tenants.db`:

| Table             | Purpose |
|-------------------|---------|
| `request_status`  | One row per orchestrate call. State (`running` / `completed` / `failed` / `canceled`), agent, conversation, started_at, completed_at, error_message, last_seq. |
| `request_events`  | Append-only event log. Each row carries `(request_id, seq, event_kind, payload_json, created_at_ms)`. Unique on `(request_id, seq)`; FK cascades from both `tenants` and `request_status`. |

`request_events.seq` is per-request monotonic, assigned by the SSE writer. The unique index prevents duplicate inserts.

## Coalescing text

The `text` SSE event fires once per provider chunk — typically dozens to hundreds per turn. Persisting each delta is wasteful: replay would emit the same fragmented stream with no fidelity gain, and the row count grows linearly with token output.

The SSE writer coalesces text deltas into ~2 KiB chunks (or one chunk per `(agent, stream_id)` boundary) before persisting. Other event kinds (`tool_call`, `file`, `token_usage`, `stream_start`, `stream_end`, `done`) persist 1:1.

The wire stream is **not** coalesced — clients see each delta in real time. Only the persisted log is batched.

## Reconnect surface

### Native: `GET /v1/requests/:id/events?since_seq=N`

```bash
curl -N \
     -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/requests/abc123/events?since_seq=42"
```

Response is `Content-Type: text/event-stream`. Each frame includes the seq as the SSE `id:` field so a re-reconnecting client can pass it back without parsing payloads:

```
id: 43
event: text
data: {"agent":"index","stream_id":1,"delta":"hello world"}

id: 44
event: tool_call
data: {"kind":"fetch","ok":true}

: heartbeat

id: 45
event: done
data: {"ok":true,"content":"…","duration_ms":12345}
```

If the run is still in `state='running'` after the backlog drains, the handler subscribes to a per-request broadcaster and continues streaming new events live until `done`. A heartbeat comment line every 30 seconds keeps reverse proxies from collapsing the connection.

If the run was already terminal at fetch time, the handler emits the persisted backlog and closes — no live tail needed.

### A2A: `tasks/resubscribe`

A2A v1 clients call `tasks/resubscribe` with `params: { id: <task_id> }` against `POST /v1/a2a/agents/:id`. The handler maps the A2A `task_id` to the same `request_id` (they're the same value at submit time) and translates each persisted event into the appropriate A2A envelope:

| Arbiter event       | A2A envelope                                              |
|---------------------|------------------------------------------------------------|
| `text`              | `TaskArtifactUpdateEvent` text-part with `append: true`    |
| `tool_call`         | `TaskArtifactUpdateEvent` data-part `{tool, ok}`           |
| `file`              | `TaskArtifactUpdateEvent` file-part with inline bytes      |
| `sub_agent_response`| `TaskArtifactUpdateEvent` data-part `{agent, depth, content}` |
| `done`              | `TaskStatusUpdateEvent` with `final: true` + final state   |
| anything else       | `TaskArtifactUpdateEvent` metadata under `x-arbiter.<kind>` |

A2A clients that aren't aware of arbiter-specific metadata silently ignore the `x-arbiter.*` artifacts.

## Startup recovery

When `ApiServer::start()` runs, any `request_status` row left in `state='running'` from a previous process must have been interrupted by a crash or kill (the running process would have transitioned the row to a terminal state on its own). The recovery sweep:

1. Selects every row where `state = 'running'`.
2. Updates them to `state = 'failed'` with `completed_at = now()` and `error_message = "request was interrupted by a server restart; reconnect to retry"`.
3. Logs the count to stderr.

Reconnecting clients hitting the resubscribe endpoint after recovery see the row as terminal and get one final synthesised `done` frame with `ok: false` so their state machines transition cleanly.

The sweep is idempotent — running it twice on the same DB is a no-op.

## Listing

`GET /v1/requests` returns the calling tenant's recent runs (state, agent, started_at, completed_at, last_seq). `GET /v1/requests/:id` returns one row's full status. Use these to discover what's resumable.

## Tenancy and trust posture

Every read enforces `tenant_id` match. A leaked `request_id` never surfaces another tenant's events. The unique `(request_id, seq)` constraint prevents replay attacks via duplicate inserts.

## See also

- [`GET /v1/requests`](../api/requests/list.md)
- [`GET /v1/requests/:id`](../api/requests/get.md)
- [`GET /v1/requests/:id/events`](../api/requests/events.md)
- [`POST /v1/requests/:id/cancel`](../api/requests-cancel.md)
- [A2A protocol](a2a.md) — `tasks/resubscribe` is the A2A counterpart
- [SSE event catalog](sse-events.md) — what each persisted event_kind means
- [Operational notes](operations.md) — recovery sweep, log retention
