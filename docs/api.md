# Arbiter HTTP API

Arbiter exposes its multi-agent orchestrator as an HTTP + Server-Sent Events API. One `POST /v1/orchestrate` drives the full agentic loop — master agent turns, delegated and parallel sub-agent calls, tool invocations, generated files — and streams the whole thing back as SSE events. Tenant authentication and usage billing are built in; a separate admin surface lets an external billing/dashboard service read the ledger.

Start with `arbiter --api --port 8080`. The default bind is `127.0.0.1`; production deployments should put TLS termination, DDoS protection, and rate limiting in a reverse proxy (nginx, caddy, cloudflare) in front of the process.

---

## Table of contents

- [Concepts](#concepts)
- [Authentication](#authentication)
- [Endpoints](#endpoints)
  - [`GET /v1/health`](#get-v1health)
  - [`POST /v1/orchestrate`](#post-v1orchestrate)
  - [`GET /v1/agents`](#get-v1agents)
  - [`GET /v1/agents/:id`](#get-v1agentsid)
  - [`POST /v1/agents/:id/chat`](#post-v1agentsidchat)
  - [`GET /v1/memory`](#get-v1memory)
  - [`GET /v1/memory/:agent_id`](#get-v1memoryagent_id)
  - [`GET /v1/admin/tenants`](#get-v1admintenants)
  - [`POST /v1/admin/tenants`](#post-v1admintenants)
  - [`GET /v1/admin/tenants/:id`](#get-v1admintenantsid)
  - [`PATCH /v1/admin/tenants/:id`](#patch-v1admintenantsid)
  - [`GET /v1/admin/usage`](#get-v1adminusage)
  - [`GET /v1/admin/usage/summary`](#get-v1adminusagesummary)
- [SSE event catalog](#sse-event-catalog)
- [Fleet streaming](#fleet-streaming)
- [Data model](#data-model)
- [Operational notes](#operational-notes)

---

## Concepts

### Tenants

A **tenant** is a named billing account. Each has an opaque API token (shown once at creation, stored as a SHA-256 digest), an optional monthly spend cap, and a rolling month-to-date usage total that resets at the start of each UTC calendar month.

Create tenants via `POST /v1/admin/tenants` or the CLI (`arbiter --add-tenant <name>`). Tenants authenticate to `/v1/orchestrate` with their token in a `Authorization: Bearer …` header.

### Money on the wire

All monetary values are **micro-cents** (µ¢), 64-bit integers, where `1 USD = 1_000_000 µ¢`. Integer math keeps fractional-cent LLM costs precise without floating-point drift.

### Billing model

For each LLM turn (master agent + every delegated and parallel sub-agent):

1. Arbiter computes the provider's list-price cost broken down by token type (plain input, output, cache reads, cache writes) using the pricing table for the model.
2. Adds a **20% markup** over that provider cost, rounded up to the nearest µ¢. Formula: `markup = ceil(provider_uc × 0.20)`, implemented as `(provider_uc * 20 + 99) / 100`.
3. The tenant is billed `provider_uc + markup_uc`.

Historical ledger rows capture the cost breakdown **as applied at the time of the call** — when pricing rates update later, past invoices don't shift.

### Caps

If a tenant has a non-zero `monthly_cap_micro_cents` and a turn would push the month-to-date total over it, arbiter cancels the in-flight orchestration and emits a `cap_exceeded` signal in both the live SSE stream and the final `done` event. Caps are enforced at turn granularity — a single turn can push past the cap, but no further turns will run.

### Fleet streaming

Every agent turn — master, delegated, or parallel child — is assigned a monotonically-increasing **`stream_id`** (the master is 0; children increment from 1). Every SSE event carries its stream id, so when the master calls `/parallel` and three sub-agents run concurrently, their `text` deltas interleave on one HTTP connection and the consumer routes each event into the right UI slot.

See [Fleet streaming](#fleet-streaming) for routing rules and event ordering guarantees.

### Agents and memory

Arbiter recognizes two kinds of agent sources:

- **Preloaded agents** live on the server's disk at `~/.arbiter/agents/*.json` and are visible to every tenant. `index` (the master orchestrator) is always present; other preloaded agents depend on what the operator installed. Discoverable via `GET /v1/agents`.
- **Inline agents** are sent in the request body as a full `agent_def` JSON blob. They exist only for that one request. Use them when your sibling service stores agent configs in its own DB and doesn't want to sync them to arbiter's disk.

**Agent memory** is the per-agent markdown file an agent writes to via `/mem write` during a turn and reads back via `/mem read` on the next. Memory is:

- **Per-tenant scoped** — `~/.arbiter/memory/t<tenant_id>/`. Tenant A's agents can never read tenant B's notes, even if they share an agent id. This is enforced in the server; there's no way to point one tenant at another's memory dir.
- **Keyed by agent id** — the file is `<agent_id>.md`. For preloaded agents the id is the filename under `agents/` (e.g., `researcher.md`). For inline agents it's whatever `agent_def.id` you pass (typically a UUID your sibling service owns).
- **Durable across requests** — memory persists on disk even when the agent is inline-defined, as long as you pass the same `agent_def.id` each time. Change the id → fresh memory; reuse the id → resumed memory.
- **Read-exposed via API** — `GET /v1/memory` lists, `GET /v1/memory/:agent_id` reads one agent's notes, `GET /v1/memory/shared` returns the pipeline scratchpad. Writes happen only through agents during a turn (via `/mem write` in the model's response); the HTTP API is read-only.

---

## Authentication

Two token kinds, both presented as `Authorization: Bearer …`:

| Prefix  | Purpose                                         | Endpoints                                |
|---------|-------------------------------------------------|------------------------------------------|
| `atr_…` | Tenant token — drives `/v1/orchestrate`, meters against the tenant's cap and ledger. | `POST /v1/orchestrate`                   |
| `adm_…` | Admin token — read/write tenants and usage data. | `/v1/admin/*`                            |

Cross-presentation is rejected: an admin token on `/v1/orchestrate` returns 401, and a tenant token on an admin route returns 401.

**Admin token provisioning.** On first `arbiter --api` start, if `$ARBITER_ADMIN_TOKEN` is unset and `~/.arbiter/admin_token` doesn't exist, arbiter generates one, writes it at mode 0600, and prints it once on stdout. Subsequent starts reuse the file. Override at runtime with the env var.

**Tenant token provisioning.** Only returned in the response to `POST /v1/admin/tenants` (HTTP API) or `arbiter --add-tenant <name>` (CLI). The database only stores the digest — if a token is lost, issue a new one.

---

## Endpoints

### `GET /v1/health`

Liveness probe. No auth. Returns `200 OK` with body `ok\n`.

```bash
curl http://arbiter.example.com/v1/health
```

---

### `POST /v1/orchestrate`

Runs one agent request end-to-end and streams the result as SSE. Tenant auth.

**Request body** (JSON):

| Field        | Type     | Required | Default   | Description                                                                                         |
|--------------|----------|----------|-----------|-----------------------------------------------------------------------------------------------------|
| `message`    | string   | yes      | —         | The prompt to send to the agent.                                                                    |
| `agent`      | string   | no       | `"index"` | Which agent to address. Any preloaded agent id or (with `agent_def`) a caller-supplied UUID.        |
| `agent_def`  | object   | no       | —         | Inline agent definition. See [Inline agents](#inline-agents). When set, overrides any preloaded agent at this id for this one request. |

**Response**: `text/event-stream`, `Connection: close`. One request per connection (no multiplexing).

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"agent":"index","message":"research gpt-5 vs claude-opus-4-7 for code review"}' \
  http://arbiter.example.com/v1/orchestrate
```

Stream ends with either a `done` event (normal completion) or an `error` event (fatal error). `duration_ms` on `done` is wall-clock from request receipt to stream close.

See the [SSE event catalog](#sse-event-catalog) for event types and shapes.

**Policy defaults:**

- **`/exec` disabled** — agents can't run shell commands on the server. An attempt returns an `ERR:` tool result so the agent adapts.
- **`/write` intercepted** — agent-generated files never hit the server's filesystem. They're streamed back as `file` events with UTF-8 content, subject to a 10 MiB per-response cap.
- **`/pane` unavailable** — pane spawning is a REPL-mode primitive; in API mode the master gets an `ERR:` and must use `/agent` (sequential) or `/parallel` (concurrent) instead.

#### Inline agents

Send a complete agent configuration in the request body to run it for one call without installing it on the server's disk. Useful when your sibling service stores agent definitions in its own database and treats arbiter as a stateless compute layer.

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
    "advisor_model": "claude-opus-4-7"
  }
}
```

**`agent_def` fields:**

| Field            | Type           | Required | Description                                                                                                        |
|------------------|----------------|----------|--------------------------------------------------------------------------------------------------------------------|
| `id`             | string         | strongly recommended | Stable caller-owned identifier, typically a UUID. **Keys the agent's memory file**: repeated requests with the same `id` share accumulated `/mem write` notes. Omit only if you don't need memory persistence. Must match `[a-zA-Z0-9_-]`, 1–64 chars. |
| `name`           | string         | yes      | Display name. Free-form.                                                                                           |
| `role`           | string         | yes      | Short role descriptor (e.g., `"code reviewer"`). Shown to the master in its agent roster.                          |
| `model`          | string         | yes      | Model string routed by arbiter's provider prefix table (`claude-*`, `openai/*`, `ollama/*`).                       |
| `goal`           | string         | yes      | What this agent is trying to accomplish. Goes into its system prompt.                                              |
| `brevity`        | string         | no       | `"lite"` \| `"full"` \| `"ultra"`. Default `"full"`.                                                               |
| `max_tokens`     | integer        | no       | Response cap per turn. Default 1024.                                                                               |
| `temperature`    | number         | no       | 0.0–2.0. Default 0.3.                                                                                              |
| `rules`          | array\<string\>| no       | Explicit behavioral constraints appended to the system prompt.                                                     |
| `capabilities`   | array\<string\>| no       | Tools this agent is designed to use. Used by the master for routing decisions.                                     |
| `mode`           | string         | no       | `""` / `"standard"` (default compressed voice) or `"writer"` (prose mode, disables compression).                   |
| `advisor_model`  | string         | no       | Higher-capability model this agent can consult via `/advise` mid-turn.                                             |
| `personality`    | string         | no       | Free-form personality overlay.                                                                                     |

**Id resolution precedence** — when multiple id sources are present, they must agree or the request fails with `400`:

1. `agent_def.id`
2. path `:id` (for `/v1/agents/:id/chat`)
3. body `agent` field
4. fallback: `"index"`

**Constraints:**

- Cannot override `"index"` — pick a different id. The master orchestrator is held as a separate runtime object and can't be replaced per-request.
- Lifetime is exactly one request. After the response completes, the orchestrator and its transient agent are destroyed. Only the agent's memory file (if `id` was set) survives.
- No agent-definition validation beyond JSON parsing — bad `model` strings, out-of-range `max_tokens`, etc., surface as upstream errors when the request is actually made.

---

### `GET /v1/agents`

List all agents visible to this tenant (master + preloaded children from the server's `agents/` dir). Tenant auth.

**Response 200:**

```json
{
  "count": 2,
  "agents": [
    {
      "id": "index",
      "name": "index",
      "role": "orchestrator",
      "model": "claude-haiku-4-5",
      "goal": "…",
      "brevity": "full",
      "max_tokens": 4096,
      "temperature": 0.3,
      "rules": ["…"],
      "capabilities": ["…"]
    },
    {
      "id": "researcher",
      "name": "researcher",
      "role": "researcher",
      "model": "claude-haiku-4-5",
      "goal": "answer one factual question in one short paragraph",
      "brevity": "bullets",
      "max_tokens": 256,
      "temperature": 0.2,
      "rules": [],
      "capabilities": ["research"]
    }
  ]
}
```

Inline agents (sent via `agent_def` in a chat request) are **not** listed here — this endpoint only reflects what's on disk. Note also that this list is the same for every tenant; agent configs themselves aren't per-tenant, only their memory is.

---

### `GET /v1/agents/:id`

Fetch one agent's full constitution. Tenant auth. Returns the same shape as a list entry. `404` if the id isn't a preloaded agent (inline-only agents from `agent_def` cannot be fetched here — they don't exist between requests).

---

### `POST /v1/agents/:id/chat`

RESTful equivalent of `POST /v1/orchestrate` with a path-bound agent id. Tenant auth + billing.

- Request body: same as `/v1/orchestrate` except the `agent` field is ignored (the path wins).
- If the body includes `agent_def`, its `id` must match the path segment.
- Response: identical SSE stream.

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"message":"write a haiku about SQLite"}' \
  http://arbiter.example.com/v1/agents/researcher/chat
```

With an inline `agent_def` (UUID-keyed memory persistence):

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{
    "message": "what did we decide about the migration last time?",
    "agent_def": {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "name": "Migration Reviewer",
      "role": "reviewer",
      "model": "claude-haiku-4-5",
      "goal": "review migrations and remember decisions"
    }
  }' \
  http://arbiter.example.com/v1/agents/550e8400-e29b-41d4-a716-446655440000/chat
```

---

### `GET /v1/memory`

List memory files for the authenticated tenant. Tenant auth.

**Response 200:**

```json
{
  "tenant_id": 1,
  "count": 2,
  "entries": [
    {
      "kind": "agent",
      "agent_id": "550e8400-e29b-41d4-a716-446655440000",
      "size": 1247,
      "modified_at": 1777058449
    },
    {
      "kind": "shared",
      "agent_id": "",
      "size": 320,
      "modified_at": 1777058020
    }
  ]
}
```

Entries are the `.md` files under `~/.arbiter/memory/t<tenant_id>/`. `kind` is `"shared"` for the pipeline scratchpad (`shared.md`) and `"agent"` for per-agent memory (`<agent_id>.md`). The endpoint returns an empty list — not a 404 — when the tenant has never triggered a memory write.

---

### `GET /v1/memory/:agent_id`

Read one agent's persistent memory. Tenant auth. Use `:agent_id = "shared"` to read the pipeline scratchpad.

**Response 200:**

```json
{
  "agent_id": "550e8400-e29b-41d4-a716-446655440000",
  "kind": "agent",
  "exists": true,
  "size": 1247,
  "content": "# researcher memory\n\n- 2026-04-24T14:00Z: looked up Paris → France\n- 2026-04-24T14:05Z: looked up Berlin → Germany\n…"
}
```

If the agent has never written memory, `exists: false` and `content: ""` — **not** a 404. Rendering a "(no memory yet)" state in your UI doesn't need a special path.

Invalid agent ids (`..`, `/`, anything outside `[a-zA-Z0-9_-]`, empty, or >64 chars) return `400 {"error":"invalid agent id"}`. Memory not configured on the server at all returns `503`.

The content is the raw markdown file — it's whatever the agent wrote via `/mem write`, including the timestamp headers arbiter's `cmd_mem_write` injects.

---

### `GET /v1/admin/tenants`

List all tenants. Admin auth.

**Response 200:**

```json
{
  "tenants": [
    {
      "id": 1,
      "name": "acme",
      "disabled": false,
      "monthly_cap_micro_cents": 25000000,
      "month_yyyymm": "2026-04",
      "month_to_date_micro_cents": 1248300,
      "created_at": 1777056438,
      "last_used_at": 1777078022
    }
  ]
}
```

---

### `POST /v1/admin/tenants`

Create a tenant. Admin auth.

**Request body:**

| Field                       | Type    | Required | Description                                             |
|-----------------------------|---------|----------|---------------------------------------------------------|
| `name`                      | string  | yes      | Display name. No uniqueness constraint — pick your own convention. |
| `cap_usd`                   | number  | no       | Monthly cap in USD. 0 or absent = unlimited.            |
| `monthly_cap_micro_cents`   | integer | no       | Same cap in µ¢. Takes precedence over `cap_usd` if both are sent. |

**Response 201:**

```json
{
  "id": 3,
  "name": "acme",
  "disabled": false,
  "monthly_cap_micro_cents": 10000000,
  "month_yyyymm": "2026-04",
  "month_to_date_micro_cents": 0,
  "created_at": 1777056438,
  "last_used_at": 0,
  "token": "atr_6c4265a8cf89b44dca6bb50090975e9201ec990a91220017b63026efd54e1638"
}
```

The `token` field is the plaintext tenant token and is **only** returned here. Save it; the database keeps only a SHA-256 digest.

---

### `GET /v1/admin/tenants/:id`

Fetch one tenant. Admin auth. Returns the same shape as list entries. `404` if the id doesn't exist.

---

### `PATCH /v1/admin/tenants/:id`

Update a tenant. Admin auth. Both fields optional; apply whichever are present.

| Field                       | Type    | Description                                                      |
|-----------------------------|---------|------------------------------------------------------------------|
| `disabled`                  | boolean | Set `true` to block new requests from this tenant (401).         |
| `monthly_cap_usd`           | number  | New monthly cap in USD. 0 = unlimited.                           |
| `monthly_cap_micro_cents`   | integer | Same cap in µ¢. Takes precedence over `monthly_cap_usd` if both. |

**Response 200**: the updated tenant object.

Cap changes take effect at the next `record_usage` call. In-flight turns that already passed the pre-flight cap check continue to completion; the new cap gates the next turn.

---

### `GET /v1/admin/usage`

Read raw usage rows, newest first. Admin auth.

**Query parameters:**

| Name        | Type    | Description                                                              |
|-------------|---------|--------------------------------------------------------------------------|
| `tenant_id` | integer | Filter to one tenant. 0 or absent = all tenants.                         |
| `since`     | integer | Epoch seconds (inclusive). 0 or absent = no lower bound.                 |
| `until`     | integer | Epoch seconds (inclusive). 0 or absent = no upper bound.                 |
| `limit`     | integer | Row cap. Default 1000, hard max 10000.                                   |

**Response 200:**

```json
{
  "count": 3,
  "entries": [
    {
      "id": 42,
      "tenant_id": 1,
      "timestamp": 1777078022,
      "model": "claude-sonnet-4-6",
      "input_tokens": 2200,
      "output_tokens": 150,
      "cache_read_tokens": 0,
      "cache_create_tokens": 500,
      "input_micro_cents": 6600,
      "output_micro_cents": 2250,
      "cache_read_micro_cents": 0,
      "cache_create_micro_cents": 1875,
      "provider_micro_cents": 10725,
      "markup_micro_cents": 2145,
      "billed_micro_cents": 12870,
      "request_id": "req-c"
    }
  ]
}
```

`provider_micro_cents` is the sum of the four `*_micro_cents` component fields. `billed_micro_cents` = `provider_micro_cents + markup_micro_cents`. The per-token-type breakdown is captured at write time so historical rows survive pricing table updates.

---

### `GET /v1/admin/usage/summary`

Pre-aggregated rollups for analytics. Admin auth. Saves the sibling service from pulling thousands of raw rows to render a chart.

**Query parameters:** same as `/v1/admin/usage`, except `limit` is ignored and `group_by` is new:

| Name       | Type   | Values                        | Default   | Description                          |
|------------|--------|-------------------------------|-----------|--------------------------------------|
| `group_by` | string | `"model"`, `"day"`, `"tenant"` | `"model"` | Bucket key for the aggregation.      |

`day` keys are `"YYYY-MM-DD"` in UTC. `tenant` keys are the tenant id as a string. Buckets are returned sorted by `provider_micro_cents` descending (biggest spenders first).

**Response 200:**

```json
{
  "group_by": "model",
  "count": 2,
  "buckets": [
    {
      "key": "claude-sonnet-4-6",
      "calls": 1,
      "input_tokens": 2200,
      "output_tokens": 150,
      "cache_read_tokens": 0,
      "cache_create_tokens": 500,
      "input_micro_cents": 6600,
      "output_micro_cents": 2250,
      "cache_read_micro_cents": 0,
      "cache_create_micro_cents": 1875,
      "provider_micro_cents": 10725,
      "markup_micro_cents": 2145,
      "billed_micro_cents": 12870
    }
  ]
}
```

**Chart recipes:**

- **Spend by model (pie):** `group_by=model`, use `billed_micro_cents` per bucket.
- **Spend over time (line):** `group_by=day&since=<30d ago>`, plot `billed_micro_cents` per key.
- **Where spend goes (stacked bar):** `group_by=model`, stack `input_micro_cents` / `output_micro_cents` / `cache_read_micro_cents` / `cache_create_micro_cents` per bucket.
- **Per-tenant rollup:** `group_by=tenant`, list tenants by `billed_micro_cents`.

---

## SSE event catalog

Every event on the `/v1/orchestrate` stream has an `event:` line and a `data:` line containing a JSON object. Events are emitted in causal order within a single stream; see [Fleet streaming](#fleet-streaming) for cross-stream ordering when `/parallel` is in play.

### Event-by-event

| Event                | When                                              | Fields                                                                                                              |
|----------------------|---------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|
| `request_received`   | Exactly once, first event on the stream.          | `agent`, `tenant`, `tenant_id`, `message` (first 200 chars, ellipsis added if truncated).                            |
| `stream_start`       | Opens each turn. Fires for master + every delegated or parallel child. | `agent`, `stream_id`, `depth` (0 = master, 1 = delegated, 2 = sub-sub).                                              |
| `agent_start`        | Just before each turn's outbound LLM request.     | `agent`, `stream_id`, `depth`.                                                                                      |
| `text`               | Each clean (tool-call lines filtered out) delta from the model. | `agent`, `stream_id`, `depth` (master only — sub-agent text events only have `agent` + `stream_id`), `delta`.         |
| `tool_call`          | After each `/cmd` (fetch, write, agent, parallel, mem, advise, exec) finishes. | `tool`, `ok`, `stream_id`, `depth`, `agent`.                                                                        |
| `file`               | Each time the agent emits a `/write` block; content is captured in-memory and forwarded here instead of written to disk. | `path`, `size`, `encoding` (always `"utf-8"`), `content`, `stream_id`, `depth`, `agent`.                             |
| `sub_agent_response` | After a delegated turn completes (depth > 0). The full turn body in one payload — useful for consumers that don't want to reconstruct from deltas. | `agent`, `stream_id`, `depth`, `content`.                                                                           |
| `token_usage`        | After each turn's billing entry lands in the ledger. | `agent`, `stream_id`, `depth`, `model`, `input_tokens`, `output_tokens`, `cache_read_tokens?`, `cache_create_tokens?`, `input_micro_cents`, `output_micro_cents`, `cache_read_micro_cents`, `cache_create_micro_cents`, `provider_micro_cents`, `billed_micro_cents`, `mtd_micro_cents`. |
| `stream_end`         | Closes each turn. Line-buffered text is flushed before this fires, so no `text` events arrive with this `stream_id` after. | `agent`, `stream_id`, `ok`.                                                                                         |
| `error`              | Recoverable errors during the request (accounting failure, cap exceeded, transient API issue). The stream continues or terminates depending on severity. | `message`, plus context fields (e.g. `mtd_micro_cents`, `cap_micro_cents` on cap-exceeded).                          |
| `done`               | Exactly once, last event. Terminal aggregate.     | `ok`, `content`, `input_tokens`, `output_tokens`, `files_bytes`, `provider_micro_cents`, `markup_micro_cents`, `billed_micro_cents`, `tenant_id`, `cap_exceeded`, `duration_ms`. On failure: `error`. |

### Ordering guarantees

- `request_received` is always first.
- `done` is always last.
- For any given `stream_id`: `stream_start` precedes every `text` / `tool_call` / `token_usage` / `sub_agent_response` carrying it, and `stream_end` follows every one of them.
- Between streams: events interleave by wall-clock. A `text` event from `stream_id: 2` may arrive between two `text` events from `stream_id: 1` if both agents are running in parallel.

### Cap-exceeded behavior

When a turn's `mtd_micro_cents` crosses `monthly_cap_micro_cents`, arbiter:

1. Emits an `error` event with `message: "monthly usage cap exceeded"`, `mtd_micro_cents`, and `cap_micro_cents`.
2. Cancels the orchestrator's in-flight API calls.
3. Lets in-progress turns complete (they're already paid for).
4. Skips all further turns.
5. Emits `done` with `cap_exceeded: true` and whatever the final `content` was.

The tenant may be slightly over cap (by one turn's worth) — this is deliberate, to avoid dropping a turn mid-generation.

---

## Fleet streaming

**Consumer routing rule:** open a UI slot on `stream_start`, route every subsequent event with matching `stream_id` to that slot, close the slot on `stream_end`. Read `depth` to decide slot layout (0 = master, 1 = delegated child, 2 = sub-sub).

### When `/parallel` is in play

The master master emits a `/parallel … /endparallel` block with N `/agent` lines inside. Arbiter:

1. Spawns one thread per child at `depth + 1`, each with a fresh `stream_id`.
2. All children start concurrently — `stream_start` for each fires near-simultaneously, in thread-start order (not necessarily input order).
3. Each child streams independently through its lifecycle. `text`, `tool_call`, `token_usage`, `sub_agent_response`, `stream_end`.
4. Arbiter joins every child thread, aggregates the results into one tool-result block, and hands it back to the master.
5. The master resumes on its original `stream_id`, emits its synthesis turn, and closes.

Example event sequence for a master + 3-way fan-out:

```
request_received
stream_start      stream_id=0 depth=0 agent="index"
text              stream_id=0 agent="index" delta="Plan: fan out three..."
text              stream_id=0 agent="index" delta="/endparallel\n"
stream_start      stream_id=1 depth=1 agent="researcher_a"
stream_start      stream_id=2 depth=1 agent="researcher_b"
stream_start      stream_id=3 depth=1 agent="researcher_c"
agent_start       stream_id=3 agent="researcher_c"
agent_start       stream_id=2 agent="researcher_b"
agent_start       stream_id=1 agent="researcher_a"
text              stream_id=3 agent="researcher_c" delta="RESULT: Rome..."
text              stream_id=2 agent="researcher_b" delta="RESULT: Berlin..."
text              stream_id=1 agent="researcher_a" delta="RESULT: Paris..."
sub_agent_response stream_id=3 agent="researcher_c"
token_usage       stream_id=3 agent="researcher_c" billed_micro_cents=2549
stream_end        stream_id=3 ok=true
sub_agent_response stream_id=2 agent="researcher_b"
token_usage       stream_id=2 agent="researcher_b" billed_micro_cents=2711
stream_end        stream_id=2 ok=true
sub_agent_response stream_id=1 agent="researcher_a"
token_usage       stream_id=1 agent="researcher_a" billed_micro_cents=2525
stream_end        stream_id=1 ok=true
tool_call         stream_id=0 tool="parallel" ok=true
token_usage       stream_id=0 agent="index" billed_micro_cents=4204
text              stream_id=0 agent="index" delta="Paris, Berlin, and Rome..."
stream_end        stream_id=0 ok=true
done              ok=true billed_micro_cents=16043
```

### Parallel safety rails

- **Duplicate `agent_id` in one `/parallel` block rejects the whole batch.** Each Agent instance has a single history vector; two threads writing to it would corrupt state. The master receives a tool-result ERR telling it to use distinct agents or sequence the calls.
- **Depth cap still holds.** Max 2 levels of delegation. A depth-2 turn cannot `/parallel`; attempts return an ERR tool result.
- **Each parallel child gets its own dedup cache.** Sibling threads fetching the same URL both fetch — we accept the duplicate over a `std::map` data race.
- **SSE writes are serialized.** A shared mutex on the wire-writer means events interleave cleanly even when N threads emit at once.

---

## Data model

### Tenant

| Field                       | Type    | Notes                                                                      |
|-----------------------------|---------|----------------------------------------------------------------------------|
| `id`                        | integer | Assigned at creation. Stable.                                              |
| `name`                      | string  | Display-only; no uniqueness constraint.                                    |
| `disabled`                  | boolean | `true` → all `/v1/orchestrate` calls return 401.                           |
| `monthly_cap_micro_cents`   | integer | 0 = unlimited.                                                              |
| `month_yyyymm`              | string  | Current billing period, e.g. `"2026-04"`. UTC calendar.                    |
| `month_to_date_micro_cents` | integer | Running total for the current period, reset on first call of a new month. |
| `created_at`                | integer | Epoch seconds.                                                             |
| `last_used_at`              | integer | Epoch seconds. 0 if the tenant has never made a call.                      |

### UsageEntry

| Field                          | Type    | Notes                                                              |
|--------------------------------|---------|--------------------------------------------------------------------|
| `id`                           | integer | Append-only. Monotonically increasing.                             |
| `tenant_id`                    | integer | FK into `tenants`.                                                 |
| `timestamp`                    | integer | Epoch seconds, write time.                                         |
| `model`                        | string  | The exact model string the request routed to.                      |
| `input_tokens`                 | integer | Total input, including cached.                                     |
| `output_tokens`                | integer |                                                                    |
| `cache_read_tokens`            | integer | Subset of input that hit a cache.                                  |
| `cache_create_tokens`          | integer | Tokens newly written to cache. Anthropic only.                     |
| `input_micro_cents`            | integer | Plain (non-cached) input cost.                                     |
| `output_micro_cents`           | integer |                                                                    |
| `cache_read_micro_cents`       | integer | Cached-input cost (typically ~10% of input rate).                  |
| `cache_create_micro_cents`     | integer | Cache-write cost (typically ~125% of input rate).                  |
| `provider_micro_cents`         | integer | Sum of the four `*_micro_cents` above.                             |
| `markup_micro_cents`           | integer | 20% of `provider_micro_cents`, rounded up.                         |
| `billed_micro_cents`           | integer | `provider_micro_cents + markup_micro_cents`.                       |
| `request_id`                   | string? | Opaque correlation id; empty if unset.                             |

---

## Operational notes

### Config files

Everything arbiter persists lives under `~/.arbiter/`:

| Path                     | Purpose                                                                         |
|--------------------------|---------------------------------------------------------------------------------|
| `api_key`                | Anthropic API key (mode 0600). Read if `$ANTHROPIC_API_KEY` is unset.           |
| `openai_api_key`         | OpenAI API key (mode 0600). Read if `$OPENAI_API_KEY` is unset.                 |
| `admin_token`            | Admin bearer token (mode 0600). Read if `$ARBITER_ADMIN_TOKEN` is unset.        |
| `tenants.db`             | SQLite ledger. WAL mode, foreign keys enforced. Schema migrates on open.        |
| `agents/*.json`          | Preloaded agent constitutions (shared across all tenants).                      |
| `memory/t<tenant_id>/*.md` | Per-tenant agent memory. `<agent_id>.md` per agent; `shared.md` scratchpad. Created on first write. Isolated per tenant. |
| `master_model`           | Override for the master agent's model.                                          |

### Deployment

Run behind a reverse proxy. TLS, rate limiting, and DDoS protection are out of scope for arbiter itself — it binds `127.0.0.1` by default specifically to encourage this layout.

**Example nginx locationblock:**

```nginx
location /v1/ {
    proxy_pass              http://127.0.0.1:8080;
    proxy_http_version      1.1;
    proxy_set_header        Host $host;
    proxy_set_header        Authorization $http_authorization;
    proxy_buffering         off;                 # critical for SSE
    proxy_read_timeout      3600;                # long LLM calls
    add_header              X-Accel-Buffering no;
}
```

`X-Accel-Buffering: no` is already set by arbiter on its SSE responses; the nginx directive here is belt-and-suspenders.

### Scaling characteristics

- **One thread per connection.** Arbiter doesn't pool; each `/v1/orchestrate` gets a fresh `Orchestrator` with fresh agent history. Sub-agents spawned by `/parallel` are additional threads within that request.
- **SQLite on the write path.** Fine for single-node deployments up to a few hundred req/min. Multi-node = swap for Postgres (schema ports cleanly).
- **No in-memory state between requests.** Agent configs are re-read from disk each time. For high-QPS deployments, put the agents directory on tmpfs or add a file-stat cache.

### File cap

Agent-generated files (via `/write`) are captured in memory and forwarded as `file` events. A per-response cap (default 10 MiB across all files in the same request) kicks in if an agent tries to flood the stream. Beyond the cap, `/write` attempts return an `ERR:` tool result and the file is dropped.

### Error response codes

| Code | Meaning                                                                                |
|------|----------------------------------------------------------------------------------------|
| 200  | Normal — for JSON admin responses, and the opening frame of an SSE orchestrate stream. |
| 201  | `POST /v1/admin/tenants` only.                                                          |
| 400  | Malformed request (bad JSON, missing required field, bad tenant id).                   |
| 401  | Missing/invalid/mismatched bearer token.                                               |
| 404  | Unknown endpoint, or tenant id not found on a PATCH/GET.                               |
| 405  | Method not allowed on an existing admin route.                                         |
| 503  | Admin endpoints called while the server has no admin token configured.                 |

Errors on a live SSE stream are delivered as `error` events, not HTTP status changes — the 200 response opened as soon as headers were written.
