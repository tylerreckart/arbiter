# Philosophy

This is the design doc, not the user manual. It explains *why* arbiter is shaped the way it is — what was prioritised, what was deliberately left out, and the tradeoffs behind the decisions you'll run into when reading the rest of the documentation.

## 1. Agents drive a small runtime

The runtime exposes a small set of commands called [**writs**](concepts/writ.md), the DSL agents emit inline in their replies. A line-buffered filter on the streaming path catches each writ, runs the handler, and feeds the result back as a tool-result block on the next turn. There is no JSON tool-use schema. No function-calling layer. The agent's *system prompt* is where behaviour lives; the runtime is small on purpose.

## 2. One binary, three shapes; local-first, network-optional

The codebase ships as a single executable with three modes — [interactive TUI](tui/index.md), [one-shot CLI](cli/send.md), [HTTP+SSE](cli/api.md) — that share storage, agents, and configuration. Per-user state lives under `~/.arbiter/`.

## 3. Streaming is the wire format

The HTTP API is built around [Server-Sent Events](concepts/sse-events.md), not request/response. The orchestration loop emits events as the agent and its sub-agents work — `text` deltas as the model streams, `tool_call` after each `/cmd` finishes, `file` when an agent writes content, `token_usage` per turn, `sub_agent_response` after delegated turns, `stream_end` per turn, `done` once at the end. The consumer reassembles the picture from events.

The protocol cares about ordering only where it has to: `request_received` is always first, `done` is always last, per-stream events are causally ordered. Cross-stream events interleave by wall-clock — and that's the *correct* behaviour, because the underlying work is concurrent.

The TUI is structurally a consumer of this same event model, even when the work runs in-process. The `text` deltas land in the scrollback buffer, the `tool_call` events drive the spinner indicator on the mid-separator, `file` events become inline file blocks. There's no "TUI mode" of the orchestration loop that's different from the API mode.

## 4. Tenants are tight; isolation is total

When `--api` is running, every request authenticates with a bearer token tied to a tenant. From there, **every read enforces `tenant_id` match**. Conversations, messages, memory entries, relations, artifacts, scratchpads, agents — none of them are visible across tenant boundaries.

## 5. Memory: soft over hard, search wants recall, browse wants precision

The [structured memory layer](concepts/structured-memory.md) makes three choices that compound into its overall shape:

`/mem invalidate <id>` sets `valid_to` rather than deleting the row. Hard-delete still exists (`DELETE /v1/memory/entries/:id`) but the *common* lifecycle is "fact retired, not erased."

The same reversibility principle shows up in artifacts (PUT-on-conflict replaces with quota math accounting for the prior size), in tenants (disable, not delete), and in conversations (cascade-delete erases artifacts and memory links — but only when you explicitly ask for the cascade).

`/mem entries type=project` is a hard filter — the agent is browsing a category, exclusion is correct. But `/mem search <query> type=project` is a different shape: the agent is trying to *find* something, and aggressive filtering loses recall. The retrieval layer treats type and tag matches as score multipliers (~30% for type, ~20% for tag) rather than `WHERE` clauses. Project entries rank higher when the caller passed `types=[project]`, but reference and learning matches still appear if they're a better lexical fit. The agent gets the answer it needed even when it over-specified the filter.

**Retrieval is layered: lexical → locality → semantic.** 

## 6. Operational scope: do the distinctive thing well; let the proxy do the rest

Arbiter binds `127.0.0.1` by default and speaks plain HTTP. There is no built-in TLS, no built-in rate limiter, no built-in DDoS protection, no built-in WAF. [Operational notes](concepts/operations.md) makes this explicit: deploy behind a reverse proxy (nginx, caddy, cloudflare).