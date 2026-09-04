# SSE event catalog

Every event on the `/v1/orchestrate` stream has an `event:` line and a `data:` line containing a JSON object. Events are emitted in causal order within a single stream; see [Fleet streaming](fleet-streaming.md) for cross-stream ordering when `/parallel` is in play.

## Event-by-event

| Event | When | Fields |
|-------|------|--------|
| `request_received` | Exactly once, first event on the stream. | `agent`, `tenant`, `tenant_id`, `message` (first 200 chars, ellipsis added if truncated), `channel?` (`"voice"` when the request asked for spoken output). |
| `intent` | Pre-dispatch classify/route on **fresh** ingress (`original_query` omitted) when the ingress agent's `intent.mode` is not `off`. After `request_received`, before `stream_start`. See [Intent](intent.md). | `kind`, `confidence`, `source`, `target_agent`, `applied`, `requested_agent`, `applied_agent`, `brief?`, `todo_seed_count`, `plan_seed_count`, `llm_used?`, `malformed?`. |
| `reconcile.progress` | Phase note on [`POST /v1/reconcile`](../api/reconcile.md). | `request_id`, `phase`, `detail?`. |
| `reconcile.delta` | Contract observation: residual vs held clauses. | `residual`, `held`, `empty`. |
| `reconcile.verification` | Test runner outcome. | `ran`, `passed`, `command`, `reason`, `exit_code`. |
| `reconcile.rollback` | Snapshot restored after failure. | `ok`, `snapshot_path`. |
| `reconcile.done` | Structured reconcile result (before the stream's `done`). | Same shape as `GET /v1/reconcile/:id`. |
| `stream_start` | Opens each turn. Fires for master + every delegated or parallel child. | `agent`, `stream_id`, `depth` (0 = master, 1 = delegated, 2 = sub-sub). |
| `agent_start` | Just before each turn's outbound LLM request. | `agent`, `stream_id`, `depth`. |
| `text` | Each clean (tool-call lines filtered out) delta from the model. Partial prose lines are emitted as they arrive; `/cmd` lines are held until a newline confirms them. After a delegation writ is parsed, a `→ delegating: …` status line is also emitted. | `agent`, `stream_id`, `depth` (master only — sub-agent text events only have `agent` + `stream_id`), `delta`. |
| `tool_call` | After each `/cmd` (fetch, search, browse, write, agent, parallel, mem, advise, exec) finishes. | `tool`, `ok`, `stream_id`, `depth`, `agent`. |
| `file` | Each time the agent emits a `/write` block; content is captured in-memory and forwarded here instead of written to disk. | `path`, `size`, `encoding` (always `"utf-8"`), `content`, `stream_id`, `depth`, `agent`. |
| `sub_agent_response` | After a delegated turn completes (depth > 0). The full turn body in one payload — useful for consumers that don't want to reconstruct from deltas. | `agent`, `stream_id`, `depth`, `content`. |
| `token_usage` | After each turn completes. | `agent`, `stream_id`, `depth`, `model`, `input_tokens`, `output_tokens`, `cache_read_tokens?`, `cache_create_tokens?`. |
| `advisor` | Every advisor interaction. Independent of `tool_call` — fires for the runtime's gate decisions (which the executor never invokes directly) and for the executor's own `/advise` consults. See [Advisor concept](advisor.md). | `agent`, `stream_id`, `kind`, `detail?`, `preview?`, `malformed?`. |
| `presence` | Always-on watcher consult after a peer's tool batch. Fires for `SILENT` and `CONTEXT`. See [Presence](presence.md). | `watcher`, `agent` (the working agent), `stream_id`, `kind` (`silent` \| `context`), `detail?`, `malformed?`. |
| `escalation` | Out-of-band advisor halt. Fires before the corresponding `stream_end` (which arrives with `ok: false`). Only fires at the originating depth — sub-agent halts bubble up via the parent's response, not via duplicate escalation events. | `agent`, `stream_id`, `reason`. |
| `stream_end` | Closes each turn. Any remaining partial line is flushed before this fires, so no `text` events arrive with this `stream_id` after. | `agent`, `stream_id`, `ok`. |
| `error` | Recoverable errors during the request (transient upstream issue). The stream continues or terminates depending on severity. | `message`, plus optional context fields (e.g. `reason`). |
| `done` | Exactly once, last event. Terminal aggregate. | `ok`, `content`, `input_tokens`, `output_tokens`, `files_bytes`, `tenant_id`, `duration_ms`, `request_id`, `conversation_id?`, `gate_approved?` (present and `true` when the runtime gate returned `CONTINUE` on the terminating turn — HTTP loop clients use this to decide whether to self-prompt for another iteration). On failure: `error`, `error_code`. Runtime taxonomy includes `advisor_halt`, `iteration_limit`, `circuit_open`, `cancelled`, and provider codes (`auth_failed`, `rate_limited`, …). When the runtime gate halted the executor, `error_code` is `"advisor_halt"` and the halt reason is also surfaced via the preceding `escalation` event. |

### `advisor` event kinds

The `kind` field disambiguates which advisor interaction fired:

| `kind` | Meaning | `detail` | `preview` |
|--------|---------|----------|-----------|
| `consult` | Executor invoked `/advise <question>`. | The question text. | — |
| `gate_continue` | Runtime gate accepted the executor's terminating turn. | — | First ~120 chars of the executor's terminating turn. |
| `gate_redirect` | Runtime gate rerouted the executor with a synthetic user turn. | The redirect guidance. | First ~120 chars of the executor's terminating turn. |
| `gate_halt` | Runtime gate halted the executor. The next event is `escalation` with the same reason, then `stream_end` with `ok: false`. | The halt reason. | First ~120 chars of the executor's terminating turn. |
| `gate_budget` | Redirect budget exhausted; the runtime synthesised a HALT to break the loop. | Reason text including the budget cap. | First ~120 chars of the executor's terminating turn. |

`malformed: true` is set on a `gate_*` event when the advisor's reply didn't parse as a valid signal. With `advisor.malformed_halts: true` (the default) the runtime promotes that to a `gate_halt`; with `false` it falls through to `gate_continue`.

## Ordering guarantees

- `request_received` is always first.
- When intent classification runs, `intent` follows `request_received` and precedes `stream_start`.
- `done` is always last.
- For any given `stream_id`: `stream_start` precedes every `text` / `tool_call` / `token_usage` / `sub_agent_response` / `advisor` / `presence` carrying it, and `stream_end` follows every one of them.
- `presence` events (when any) arrive after the tool batch they reviewed and before the working agent's next `text` deltas.
- For an advisor halt, the order on a given `stream_id` is: `advisor` (`kind: gate_halt`) → `escalation` → `stream_end` (`ok: false`).
- Between streams: events interleave by wall-clock. A `text` event from `stream_id: 2` may arrive between two `text` events from `stream_id: 1` if both agents are running in parallel.

## A2A streaming uses a different event shape

Spec-compatible A2A clients hit [`POST /v1/a2a/agents/:id`](../api/a2a/dispatch.md) and receive [Agent2Agent v1.0](a2a.md) `TaskStatusUpdateEvent` / `TaskArtifactUpdateEvent` frames inside JSON-RPC envelopes — not the arbiter-native events documented above. The two surfaces share the same orchestrator under the hood; the A2A handler translates internal events into spec-compliant frames at the wire boundary. See [A2A protocol → streaming event mapping](a2a.md#streaming-event-mapping) for the table.

## See also

- [A2A protocol](a2a.md) — the Agent2Agent counterpart to this catalog.
- [Fleet streaming](fleet-streaming.md)
- [Advisor](advisor.md) — gate signal grammar, modes, redirect budget.
- [Presence](presence.md) — always-on peer review and mid-turn injection.
- [Intent](intent.md) — pre-dispatch classify/route.
- [Reconcile](reconcile.md) — desired-end-state contract, tests, rollback.
- [Voice](voice.md) — spoken constitution register and `channel: "voice"` for TTS bridges.
- [`POST /v1/orchestrate`](../api/orchestrate.md)
- [`POST /v1/conversations/:id/messages`](../api/conversations/messages-post.md)
