# SSE event catalog

Every event on the `/v1/orchestrate` stream has an `event:` line and a `data:` line containing a JSON object. Events are emitted in causal order within a single stream; see [Fleet streaming](fleet-streaming.md) for cross-stream ordering when `/parallel` is in play.

## Event-by-event

| Event | When | Fields |
|-------|------|--------|
| `request_received` | Exactly once, first event on the stream. | `agent`, `tenant`, `tenant_id`, `message` (first 200 chars, ellipsis added if truncated). |
| `stream_start` | Opens each turn. Fires for master + every delegated or parallel child. | `agent`, `stream_id`, `depth` (0 = master, 1 = delegated, 2 = sub-sub). |
| `agent_start` | Just before each turn's outbound LLM request. | `agent`, `stream_id`, `depth`. |
| `text` | Each clean (tool-call lines filtered out) delta from the model. Master text is suppressed during delegation iterations — only `→ delegating: …` status lines reach the wire until the synthesis turn. | `agent`, `stream_id`, `depth` (master only — sub-agent text events only have `agent` + `stream_id`), `delta`. |
| `tool_call` | After each `/cmd` (fetch, search, browse, write, agent, parallel, mem, advise, exec) finishes. | `tool`, `ok`, `stream_id`, `depth`, `agent`. |
| `file` | Each time the agent emits a `/write` block; content is captured in-memory and forwarded here instead of written to disk. | `path`, `size`, `encoding` (always `"utf-8"`), `content`, `stream_id`, `depth`, `agent`. |
| `sub_agent_response` | After a delegated turn completes (depth > 0). The full turn body in one payload — useful for consumers that don't want to reconstruct from deltas. | `agent`, `stream_id`, `depth`, `content`. |
| `token_usage` | After each turn's token tally is fired off to the billing service. | `agent`, `stream_id`, `depth`, `model`, `input_tokens`, `output_tokens`, `cache_read_tokens?`, `cache_create_tokens?`. Cost figures are not included — the billing service's ledger is authoritative. |
| `stream_end` | Closes each turn. Line-buffered text is flushed before this fires, so no `text` events arrive with this `stream_id` after. | `agent`, `stream_id`, `ok`. |
| `error` | Recoverable errors during the request (billing-service denial, transient upstream issue). The stream continues or terminates depending on severity. | `message`, plus context fields (e.g. `reason`, `estimated_cost_micro_cents`, `total_budget_micro_cents` on a billing-service denial). |
| `done` | Exactly once, last event. Terminal aggregate. | `ok`, `content`, `input_tokens`, `output_tokens`, `files_bytes`, `tenant_id`, `duration_ms`, `request_id`, `conversation_id?`. On failure: `error`. |

## Ordering guarantees

- `request_received` is always first.
- `done` is always last.
- For any given `stream_id`: `stream_start` precedes every `text` / `tool_call` / `token_usage` / `sub_agent_response` carrying it, and `stream_end` follows every one of them.
- Between streams: events interleave by wall-clock. A `text` event from `stream_id: 2` may arrive between two `text` events from `stream_id: 1` if both agents are running in parallel.

## Billing-service pre-flight denial

When the configured billing service (via `ARBITER_BILLING_URL`) rejects the request, the runtime emits:

1. An `error` event with `reason ∈ {tenant_suspended, tenant_disabled, insufficient_budget}` and a human-readable `message`.
2. A `done` event with `ok: false`.

No turns run. A transport error to the billing service fails open — the runtime proceeds rather than blocking on a billing-service blip.

## See also

- [Fleet streaming](fleet-streaming.md)
- [`POST /v1/orchestrate`](../orchestrate.md)
- [`POST /v1/conversations/:id/messages`](../conversations/messages-post.md)
