# `GET /v1/requests/:id/events`

**Auth:** tenant — _Status:_ stable

SSE replay-and-tail for a previously-started orchestrate run. Streams the persisted backlog after `since_seq`, then live-tails the run if it's still in flight.

This is how a client whose connection dropped mid-stream resumes without losing what was emitted while it was offline. It's also how a second client (a side-by-side debug pane, a CLI tail) attaches to a running stream by id.

## Request

### Query parameters

| Param        | Description                                                                  |
|--------------|------------------------------------------------------------------------------|
| `since_seq`  | Replay only events with `seq > since_seq` (default `0` ⇒ full replay). Pass the highest seq your client has seen so the server skips what you already have. |

```bash
curl -N \
     -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/requests/a1b2c3d4e5f60718/events?since_seq=42"
```

The `-N` flag (no buffering) is essential — without it `curl` may buffer the SSE response.

## Response

`Content-Type: text/event-stream`. Each frame mirrors one persisted event:

```
id: 43
event: text
data: {"agent":"index","stream_id":1,"delta":"hello world"}

id: 44
event: tool_call
data: {"kind":"fetch","ok":true}

id: 45
event: file
data: {"path":"output/report.md","content":"…","mime_type":"text/markdown"}

: heartbeat

id: 46
event: done
data: {"ok":true,"content":"…","duration_ms":12345}
```

The `id:` field carries the persisted `seq` so a re-reconnecting client can pass it as `since_seq` without parsing the JSON payload.

### Replay-then-tail behavior

1. The handler verifies the `request_id` exists for the tenant.
2. It queries `request_events` for rows with `seq > since_seq` and streams them in seq-asc order. Pagination is internal (1000 events per fetch); the wire stream is continuous.
3. After draining the backlog, it re-checks `request_status.state`:
   - **Terminal** (`completed`, `failed`, `canceled`): close the connection.
   - **Running**: subscribe to the per-request broadcaster; stream new events as they're persisted; close on the terminal `done` event.
4. A `: heartbeat` comment line every 30 seconds keeps reverse proxies from collapsing an idle live-tail.

### Coalescing note

The `text` event in the persisted log is **coalesced** — each row aggregates ~2 KiB of consecutive text deltas (or one row per `(agent, stream_id)` boundary) before persistence. Replaying a stream produces fewer, larger text frames than the live wire stream did. The assembled string is identical; only the chunk granularity differs.

Other event kinds (`tool_call`, `file`, `token_usage`, `stream_start`, `stream_end`, `done`) are persisted 1:1.

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Request not found for this tenant. |

If the run was interrupted by a server crash before this endpoint was called, the recovery sweep at startup will have flipped its state to `failed` with `error_message="request was interrupted by a server restart; reconnect to retry"`. The endpoint replays the backlog and closes; the client sees the persisted events plus a final synthesised `done` frame with `ok: false`.

## See also

- [`GET /v1/requests`](list.md), [`GET /v1/requests/:id`](get.md)
- [`POST /v1/requests/:id/cancel`](../requests-cancel.md)
- [Durable in-flight execution](../../concepts/durable-execution.md)
- [SSE event catalog](../../concepts/sse-events.md) — what each `event_kind` means.
