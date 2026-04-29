# Arbiter HTTP API

Arbiter exposes its multi-agent orchestrator as an HTTP + Server-Sent Events API. One `POST /v1/orchestrate` drives the full agentic loop — master agent turns, delegated and parallel sub-agent calls, tool invocations, generated files — and streams the whole thing back as SSE events. Tenant authentication and usage billing are built in; a separate admin surface lets an external billing/dashboard service read the ledger.

Start with `arbiter --api --port 8080`. The default bind is `127.0.0.1`; production deployments should put TLS termination, DDoS protection, and rate limiting in a reverse proxy (nginx, caddy, cloudflare) in front of the process.

---

## Table of contents

- [Concepts](#concepts)
- [Authentication](#authentication)
- [Endpoints](#endpoints)
  - [`GET /v1/health`](#get-v1health)
  - [`GET /v1/models`](#get-v1models)
  - [`POST /v1/orchestrate`](#post-v1orchestrate)
  - [`POST /v1/requests/:id/cancel`](#post-v1requestsidcancel)
  - [`GET /v1/agents`](#get-v1agents)
  - [`POST /v1/agents`](#post-v1agents)
  - [`GET /v1/agents/:id`](#get-v1agentsid)
  - [`PATCH /v1/agents/:id`](#patch-v1agentsid)
  - [`DELETE /v1/agents/:id`](#delete-v1agentsid)
  - [`POST /v1/agents/:id/chat`](#post-v1agentsidchat)
  - [`POST /v1/conversations`](#post-v1conversations)
  - [`GET /v1/conversations`](#get-v1conversations)
  - [`GET /v1/conversations/:id`](#get-v1conversationsid)
  - [`PATCH /v1/conversations/:id`](#patch-v1conversationsid)
  - [`DELETE /v1/conversations/:id`](#delete-v1conversationsid)
  - [`GET /v1/conversations/:id/messages`](#get-v1conversationsidmessages)
  - [`POST /v1/conversations/:id/messages`](#post-v1conversationsidmessages)
  - [`GET /v1/memory`](#get-v1memory)
  - [`GET /v1/memory/:agent_id`](#get-v1memoryagent_id)
  - [Structured memory](#structured-memory)
    - [`POST /v1/memory/entries`](#post-v1memoryentries)
    - [`GET /v1/memory/entries`](#get-v1memoryentries)
    - [`GET /v1/memory/entries/:id`](#get-v1memoryentriesid)
    - [`PATCH /v1/memory/entries/:id`](#patch-v1memoryentriesid)
    - [`DELETE /v1/memory/entries/:id`](#delete-v1memoryentriesid)
    - [`POST /v1/memory/relations`](#post-v1memoryrelations)
    - [`GET /v1/memory/relations`](#get-v1memoryrelations)
    - [`PATCH /v1/memory/relations/:id`](#patch-v1memoryrelationsid)
    - [`DELETE /v1/memory/relations/:id`](#delete-v1memoryrelationsid)
    - [`GET /v1/memory/graph`](#get-v1memorygraph)
    - [`GET /v1/memory/proposals`](#get-v1memoryproposals)
    - [Agent access](#agent-access-to-structured-memory)
    - [Proposal queue](#proposal-queue)
  - [`GET /v1/admin/tenants`](#get-v1admintenants)
  - [`POST /v1/admin/tenants`](#post-v1admintenants)
  - [`GET /v1/admin/tenants/:id`](#get-v1admintenantsid)
  - [`PATCH /v1/admin/tenants/:id`](#patch-v1admintenantsid)
  - [`GET /v1/admin/usage`](#get-v1adminusage)
  - [`GET /v1/admin/usage/summary`](#get-v1adminusagesummary)
- [MCP servers](#mcp-servers)
- [Web search](#web-search)
- [Artifacts](#artifacts)
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

The HTTP API does **not** load agent `.json` definitions from disk. Three sources of truth for an agent's constitution coexist:

- **`index`** — the built-in master orchestrator. Always available; needs no `agent_def`. Routes work, delegates, and synthesises across other agents.
- **Stored agents** — per-tenant catalog persisted on the server via [`POST /v1/agents`](#post-v1agents). Once registered, an agent is referenceable by `agent_id` from any subsequent `/v1/orchestrate`, `/v1/agents/:id/chat`, conversation thread, or — most usefully — from `/agent` and `/parallel` slash commands inside another agent's turn. Update wholesale with `PATCH`, remove with `DELETE`. The front-end is the canonical owner; the server stores the blob it last received.
- **Inline agents** — sent in the request body as a full `agent_def` JSON blob. They exist only for the request (or, when supplied to `POST /v1/conversations`, for the lifetime of that conversation row). Use these for ephemeral one-offs and mid-thread overrides without touching the catalog.

**Resolution order on `/v1/orchestrate`** when picking the constitution to install:

1. Inline `agent_def` in the request body (always wins on id collision).
2. The conversation's snapshotted `agent_def` (for `POST /v1/conversations/:id/messages` follow-ups).
3. The tenant's stored agents catalog (looked up by `agent_id`).
4. The built-in `index` master.

The orchestrator pre-loads **every** stored agent for the tenant before the turn runs, so `/agent <stored_id> <msg>` and `/parallel`-fan-outs to stored siblings work without inline definitions. Inline `agent_def` still wins on a colliding id, so you can override one stored agent mid-turn without disturbing the rest of the catalog.

Empty body + non-`index` agent + no snapshot + no stored row ⇒ a clear SSE error tells the caller to register the agent or supply `agent_def`.

**Agent memory** is the per-agent markdown file an agent writes to via `/mem write` during a turn and reads back via `/mem read` on the next. Memory is:

- **Per-tenant scoped** — `~/.arbiter/memory/t<tenant_id>/`. Tenant A's agents can never read tenant B's notes, even if they share an agent id. This is enforced in the server; there's no way to point one tenant at another's memory dir.
- **Keyed by agent id** — the file is `<agent_id>.md`. The id is whatever `agent_def.id` you pass (typically a UUID your sibling service owns).
- **Durable across requests** — memory persists on disk as long as you pass the same `agent_def.id` each time. Change the id → fresh memory; reuse the id → resumed memory.
- **Read-exposed via API** — `GET /v1/memory` lists, `GET /v1/memory/:agent_id` reads one agent's notes, `GET /v1/memory/shared` returns the pipeline scratchpad. Writes happen only through agents during a turn (via `/mem write` in the model's response); the HTTP API is read-only for the file-scratchpad surface. (The structured-memory surface at `/v1/memory/entries` does support full CRUD — see [Structured memory](#structured-memory).)

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

### `GET /v1/models`

List the models arbiter knows how to price + route. Tenant auth. Powers the frontend's model picker.

**Response 200:**

```json
{
  "count": 41,
  "models": [
    {
      "id": "claude-opus-4-7",
      "provider": "anthropic",
      "input_per_mtok_usd": 5.0,
      "output_per_mtok_usd": 25.0,
      "cache_read_per_mtok_usd": 0.5,
      "cache_create_per_mtok_usd": 6.25,
      "supports_caching": true
    }
  ]
}
```

`id` matches what you pass in `agent_def.model` (or as the model on a preloaded agent). `provider` is inferred from the id (`anthropic`, `openai`, `ollama`). `*_per_mtok_usd` are list-price rates per 1M tokens before arbiter's 20% markup. `supports_caching` reflects whether the family bills cache reads/writes separately.

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

### `POST /v1/requests/:id/cancel`

Cancel an in-flight orchestration. Tenant auth. The `:id` is the `request_id` from the SSE `request_received` event of the call you want to stop. Wires to a Stop button in the UI.

**Response 200** if the request was found and cancellation was issued:

```json
{ "request_id": "a889e53a7211eefa", "cancelled": true }
```

**Response 404** if no in-flight request matches (already finished, never existed, or belongs to a different tenant — the response is deliberately the same in all three cases so an attacker can't enumerate other tenants' request ids):

```json
{ "request_id": "a889e53a7211eefa", "cancelled": false, "reason": "no in-flight request with that id" }
```

Cancellation is best-effort — a turn that's already returned from the LLM but hasn't been billed yet may complete; the next turn won't start. The cancelled stream still emits a final `done` event (with `ok: false` typically) so consumers see a clean close.

---

### `GET /v1/agents`

List the agents visible to this tenant — the built-in `index` master plus every agent stored for this tenant via `POST /v1/agents`. Tenant auth. Newest `updated_at` first; `index` is always the head of the list.

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
      "name": "Researcher",
      "role": "researcher",
      "model": "claude-sonnet-4-6",
      "goal": "answer one factual question in one short paragraph",
      "brevity": "bullets",
      "max_tokens": 256,
      "temperature": 0.2,
      "rules": [],
      "capabilities": ["research"],
      "created_at": 1777060001,
      "updated_at": 1777060001
    }
  ]
}
```

Stored agents carry `created_at` and `updated_at`; the built-in `index` does not (its constitution is server-controlled and immutable from the API).

Inline agents (passed as `agent_def` on a single `/v1/orchestrate` call) are **not** listed here — they exist only for that one call and aren't persisted. Persist with `POST /v1/agents` if you want catalog visibility, snapshotting onto conversations, and `/agent`/`/parallel` references by id.

---

### `POST /v1/agents`

Create a stored agent for this tenant. Tenant auth.

**Request body** — either a bare constitution or wrapped under `agent_def`:

```json
{
  "id": "researcher",
  "name": "Researcher",
  "role": "researcher",
  "model": "claude-sonnet-4-6",
  "goal": "answer one factual question in one short paragraph",
  "brevity": "bullets",
  "max_tokens": 256,
  "temperature": 0.2,
  "rules": [],
  "capabilities": ["research"]
}
```

`id` is required and caller-chosen — typically a UUID owned by the sibling service. It must match `[A-Za-z0-9_-]{1,64}` (it becomes a path component for the file scratchpad and a token in slash-command syntax) and is unique per tenant. Two tenants may independently use the same `id`.

**Response 201:** the stored agent with `created_at`/`updated_at`. The full blob is persisted; the response is rendered by re-parsing it through `Constitution::from_json`, so what comes back is exactly what's stored.

**Errors:**

- `400 {"error": "..."}` — body isn't a valid constitution (`Constitution::from_json` failed), `id` is missing/malformed, or `id == "index"` (reserved for the built-in master).
- `409 {"error": "agent '<id>' already exists for this tenant", "existing": { ...AgentRecord... }}` — duplicate `id` for this tenant. Use `PATCH` to replace.

---

### `GET /v1/agents/:id`

Fetch one agent's constitution. Tenant auth. `:id = "index"` returns the built-in master; any other id is looked up against the tenant's stored catalog. `404 {"error": "no agent '<id>' for this tenant"}` if not found.

---

### `PATCH /v1/agents/:id`

**Wholesale replace** of a stored agent's `agent_def` blob. Tenant auth. The body has the same shape as `POST /v1/agents`; if the body's `id` is set it must match the path `:id`. The new blob is validated through `Constitution::from_json` before the row is touched, so a bad body never corrupts a stored agent.

**Response 200:** the updated agent, with `created_at` preserved and `updated_at` bumped.

**Errors:**

- `400` — body isn't a valid constitution, body `id` doesn't match path, or `:id == "index"` (the master cannot be modified).
- `404` — no agent with this id for this tenant.

There is no field-level merge — the front-end is the source of truth and resends the full blob on every change. (For mid-thread one-off overrides without persisting, use inline `agent_def` on the orchestrate call instead.)

---

### `DELETE /v1/agents/:id`

Remove a stored agent. Tenant auth. `:id == "index"` returns 400 (the master is built-in); a missing id returns 404. **Response 200:** `{ "deleted": true }`.

Deleting an agent does not cascade to conversations that snapshotted its `agent_def` — those threads keep working off the snapshot. Stored memory file scratchpads at `~/.arbiter/memory/t<tenant>/<agent_id>.md` are also kept (they're tied to the id, not the catalog row).

---

### `POST /v1/agents/:id/chat`

RESTful equivalent of `POST /v1/orchestrate` with a path-bound agent id. Tenant auth + billing.

- Request body: same as `/v1/orchestrate` except the `agent` field is ignored (the path wins).
- The path `:id` resolves through the same chain as `/v1/orchestrate` — inline `agent_def` first, then the tenant's stored catalog, then the built-in `index`. So you can hit a stored agent with no body beyond `message`.
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

## Conversations

A conversation is a stored thread of messages between a tenant's user and one agent. The frontend uses these for the left-rail "previous chats" list, history reload, and persistent context across page navigations. Behind the scenes:

- **Server owns the history.** When the user sends a message, arbiter loads prior messages from the DB into the agent's context, runs one turn (with full streaming and billing as described in [`/v1/orchestrate`](#post-v1orchestrate)), and persists both the user's message and the assistant's response.
- **Tenant-scoped.** Every endpoint enforces `tenant_id` match — id leaks across tenants surface as 404, never as data exposure.
- **Cascading delete.** `DELETE /v1/conversations/:id` drops all messages too (FK with `ON DELETE CASCADE`).
- **Inline agent snapshots.** If you create a conversation with an `agent_def`, the full definition JSON is stored on the conversation row. The thread keeps working even if your sibling service later drops or changes the agent definition.

### `POST /v1/conversations`

Create a new conversation. Tenant auth.

**Request body:**

| Field        | Type    | Required | Default   | Description                                                              |
|--------------|---------|----------|-----------|--------------------------------------------------------------------------|
| `title`      | string  | no       | `""`      | Display title. Empty until you set one (or auto-titling is added later). |
| `agent_id`   | string  | no       | `"index"` | Which agent this conversation talks to.                                  |
| `agent_def`  | object  | no       | —         | Inline agent definition (same shape as in chat requests). Required when `agent_id != "index"` if you want follow-ups to work without re-sending it on every turn — the definition is snapshotted onto the conversation row at create time and reused on every subsequent `POST /v1/conversations/:id/messages`. |

**Response 201:** the new `Conversation` object — `id`, `title`, `agent_id`, `created_at`, `updated_at`, `message_count: 0`, `archived: false`, and `agent_def` if provided.

```bash
curl -H "Authorization: Bearer atr_…" -H "Content-Type: application/json" \
  -d '{"title":"Q3 planning","agent_id":"index"}' \
  http://arbiter.example.com/v1/conversations
```

---

### `GET /v1/conversations`

List the tenant's conversations, newest-updated first. Tenant auth.

**Query parameters:**

| Name                | Type    | Description                                                                |
|---------------------|---------|----------------------------------------------------------------------------|
| `before_updated_at` | integer | Epoch seconds. Returns conversations updated strictly before this. Use the previous page's last `updated_at` to paginate backward. |
| `limit`             | integer | Page size. Default 50, max 200.                                            |

**Response 200:** `{ "count": N, "conversations": [...] }`.

---

### `GET /v1/conversations/:id`

Fetch one conversation's metadata. Tenant auth. `404` if the id doesn't exist or belongs to another tenant.

---

### `PATCH /v1/conversations/:id`

Update a conversation. Tenant auth. Both fields optional.

| Field      | Type    | Description                                |
|------------|---------|--------------------------------------------|
| `title`    | string  | New display title.                          |
| `archived` | boolean | Hide from default list views without deletion (the list endpoint still returns archived rows; clients filter). |

**Response 200:** the updated `Conversation` object. `404` if not found.

---

### `DELETE /v1/conversations/:id`

Permanently delete a conversation and all its messages. Tenant auth.

**Response 200:** `{ "deleted": true }`. `404` if not found.

---

### `GET /v1/conversations/:id/messages`

List messages in a conversation, oldest first (chat order, ready to render). Tenant auth.

**Query parameters:**

| Name       | Type    | Description                                                          |
|------------|---------|----------------------------------------------------------------------|
| `after_id` | integer | Return messages with `id > after_id`. Useful for incremental polling. |
| `limit`    | integer | Page size. Default 200, max 500.                                      |

**Response 200:**

```json
{
  "conversation_id": 1,
  "count": 4,
  "messages": [
    {
      "id": 1,
      "conversation_id": 1,
      "role": "user",
      "content": "what's a fanout in DSLs?",
      "input_tokens": 0,
      "output_tokens": 0,
      "billed_micro_cents": 0,
      "created_at": 1777088746,
      "request_id": "a889e53a7211eefa"
    },
    {
      "id": 2,
      "conversation_id": 1,
      "role": "assistant",
      "content": "A fanout is …",
      "input_tokens": 1234,
      "output_tokens": 567,
      "billed_micro_cents": 4823,
      "created_at": 1777088752,
      "request_id": "a889e53a7211eefa"
    }
  ]
}
```

User messages have zero tokens/billing (they're the input, not the cost). Assistant messages carry the full request totals. `request_id` correlates with the `usage_log` row created during that turn.

---

### `POST /v1/conversations/:id/messages`

Send a user message and stream the assistant's reply. Tenant auth + billing. Same SSE response shape as `/v1/orchestrate` plus a `conversation_id` field on the `done` event.

**Request body:**

| Field        | Type     | Required | Description                                                |
|--------------|----------|----------|------------------------------------------------------------|
| `message`    | string   | yes      | The new user turn.                                         |
| `agent_def`  | object   | no       | Override the conversation's snapshotted agent for this one turn (rare — usually a follow-up should just send `message`). When omitted, the conversation's snapshot from create time is reused. |

**What happens server-side:**

1. Conversation lookup — `404` if missing or wrong tenant. Validation surfaces as a clean JSON error before the SSE stream opens.
2. The conversation's snapshotted `agent_def` is applied to the orchestrator (unless the request body supplied its own, which wins). This is what makes follow-ups work without re-sending the agent definition: the snapshot is the source of truth.
3. Prior messages loaded from the DB and replayed into the agent's history (capped at the most recent 100 turns to keep request payload bounded).
4. The user's `message` is persisted with the `request_id` issued for this stream.
5. The orchestrator runs and streams events exactly as `/v1/orchestrate` would.
6. On a successful `done`, the assistant's full response is persisted alongside the request's billing totals (`input_tokens`, `output_tokens`, `billed_micro_cents`).
7. On failure (`done.ok = false`), the assistant message is not persisted — only the user message remains. Retry is safe.

```bash
curl -N \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"message":"and what about cache writes?"}' \
  http://arbiter.example.com/v1/conversations/1/messages
```

The `request_id` from this call's `request_received` event is the handle to pass to [`POST /v1/requests/:id/cancel`](#post-v1requestsidcancel) if the user clicks Stop.

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

## Structured memory

Two memory surfaces ship side-by-side under `/v1/memory`:

- **File scratchpads** (`GET /v1/memory`, `GET /v1/memory/:agent_id`) — the legacy per-agent markdown documented above. **Read-only** over HTTP; agents write via `/mem write` during a turn.
- **Structured memory** (`/v1/memory/entries`, `/v1/memory/relations`, `/v1/memory/graph`) — typed nodes and directed labeled edges in SQLite, with full CRUD over HTTP. Backs the frontend graph UI.

The two sub-systems do **not** share storage. An entry is not a parsed agent scratchpad; an agent's `/mem write` does not create entries.

Agents can both **read** and **propose** into structured memory in real time during a turn:

- Reads (`/mem entries`, `/mem entry`, `/mem search`) only ever surface the **curated graph** — entries and relations with `status="accepted"`.
- Writes from agents land in the **proposal queue** (`status="proposed"`) via `/mem propose entry` and `/mem propose link`. Proposed rows are invisible to every read path — both HTTP and the agent's own slash commands — until a human reviewer accepts them through the HTTP surface ([Proposal queue](#proposal-queue)).

That split is the safety boundary: agents never directly mutate the curated graph their future selves will read.

### Closed enums

These are validated server-side and rejected with `400 {"error":"..."}` if violated. Adding values is a coordinated frontend+API change.

| Field             | Allowed values                                                                          |
|-------------------|-----------------------------------------------------------------------------------------|
| `entry.type`      | `user`, `feedback`, `project`, `reference`, `learning`, `context`                       |
| `relation`        | `relates_to`, `refines`, `contradicts`, `supersedes`, `supports`                        |

### Per-entry constraints

| Field     | Constraint                                                              |
|-----------|-------------------------------------------------------------------------|
| `title`   | Non-empty, ≤ 200 chars                                                  |
| `content` | ≤ 64 KiB                                                                |
| `source`  | ≤ 200 chars                                                             |
| `tags`    | JSON array of strings; each tag 1–64 chars; up to 32 tags. Always present in responses; pass `[]` (or omit) for none. |

### Per-relation constraints

- `source_id != target_id` — self-loops return 400 `"self-loops not allowed"`.
- Both endpoints must belong to the calling tenant — otherwise 400 `"entries belong to different tenants"`. (We don't return 404-per-side because that would either leak whether the other tenant's id exists, or be uselessly ambiguous about which side is missing.)
- Relations are **directed and per-type**. The same pair can have multiple relations of different kinds; the same `(source, target, relation)` triple cannot exist twice — a duplicate returns 409 `{"error":"...", "existing_id": N}`. Symmetric relations like `contradicts` are still stored directed; clients should dedupe for display.

### `POST /v1/memory/entries`

Create an entry. Tenant auth.

**Request body:**

| Field     | Type            | Required | Description                              |
|-----------|-----------------|----------|------------------------------------------|
| `type`    | string (enum)   | yes      | One of the six entry types.              |
| `title`   | string          | yes      | 1–200 chars.                             |
| `content` | string          | no       | ≤ 64 KiB. Defaults to `""`.              |
| `source`  | string          | no       | Free-form provenance. ≤ 200 chars.       |
| `tags`    | array<string>   | no       | Defaults to `[]`.                        |

**Response 201:** the new `Entry`. Entries created via this endpoint always land in `status="accepted"` (the curated graph). Agent-originated entries go into the proposal queue with `status="proposed"` — see [Proposal queue](#proposal-queue).

```json
{
  "id": 42,
  "tenant_id": 1,
  "type": "project",
  "title": "Frontend graph",
  "content": "Force-directed view of memory.",
  "source": "planning",
  "tags": ["scope", "hub"],
  "status": "accepted",
  "created_at": 1777058449,
  "updated_at": 1777058449
}
```

---

### `GET /v1/memory/entries`

List entries for the authenticated tenant, newest `updated_at` first. Tenant auth. **Curated graph only** — proposals (`status="proposed"`) are hidden from this endpoint; fetch them via `GET /v1/memory/proposals`.

**Query parameters** (all optional):

| Name                | Type        | Description                                                       |
|---------------------|-------------|-------------------------------------------------------------------|
| `type`              | csv string  | `?type=project,reference` — OR-filter on the enum. Unknown values reject 400. |
| `tag`               | string      | Single-tag substring match against the serialized JSON.           |
| `q`                 | string      | LIKE-match on `title` + `content`. Case follows SQLite's default. |
| `since`             | epoch s     | `created_at >= since`.                                            |
| `before_updated_at` | epoch s     | `updated_at < before_updated_at`. Use for cursor pagination.      |
| `limit`             | int         | Default 50, hard max 200.                                         |

**Response 200:** `{ "entries": [...], "count": N }`.

---

### `GET /v1/memory/entries/:id`

Read one entry. Tenant auth. `404` if the id doesn't exist, belongs to another tenant, or is still in the proposal queue (`status="proposed"`) — proposals are not addressable through the curated read path. To inspect a proposed entry, list it via `GET /v1/memory/proposals`.

---

### `PATCH /v1/memory/entries/:id`

Update any subset of `{title, content, source, tags, type, status}`. Tenant auth. `created_at` is immutable; `updated_at` is bumped on a successful change. `404` if not found.

**`status` is the proposal-queue acceptance lever.** The only valid value is `"accepted"` — sending it on a `status="proposed"` entry promotes the row into the curated graph. Demoting an accepted entry back to proposed is not supported (400). To reject a proposal, `DELETE` it. Promoting an already-accepted entry returns `409 {"error": "entry is not in 'proposed' status — nothing to accept."}`.

**Response 200:** the updated `Entry`.

---

### `DELETE /v1/memory/entries/:id`

Permanently delete an entry. **Cascades to relations** with this entry as either endpoint (regardless of status). Tenant auth. This is also the **rejection path for proposed entries** — there is no separate `/reject` verb.

**Response 200:** `{ "deleted": true }`.

---

### `POST /v1/memory/relations`

Create a directed labeled edge. Tenant auth.

**Request body:**

| Field        | Type          | Required | Description                          |
|--------------|---------------|----------|--------------------------------------|
| `source_id`  | int           | yes      | Entry id this edge points *from*.    |
| `target_id`  | int           | yes      | Entry id this edge points *to*.      |
| `relation`   | string (enum) | yes      | One of the five relations.           |

**Response 201:** the new `Relation`. Relations created via this endpoint always land in `status="accepted"`. Agent-originated edges go into the proposal queue with `status="proposed"` — see [Proposal queue](#proposal-queue).

```json
{
  "id": 7,
  "tenant_id": 1,
  "source_id": 42,
  "target_id": 43,
  "relation": "supports",
  "status": "accepted",
  "created_at": 1777058500
}
```

**409** with `{"error": "relation already exists", "existing_id": N}` on a duplicate `(source, target, relation)` triple.

---

### `GET /v1/memory/relations`

List relations for the authenticated tenant. Tenant auth. **Curated graph only** — proposed edges (`status="proposed"`) are hidden; fetch them via `GET /v1/memory/proposals`.

**Query parameters** (all optional):

| Name        | Type   | Description                       |
|-------------|--------|-----------------------------------|
| `source_id` | int    | Filter to edges from this entry.  |
| `target_id` | int    | Filter to edges to this entry.    |
| `relation`  | string | Filter to one relation kind.      |
| `limit`     | int    | Default 200, hard max 1000.       |

**Response 200:** `{ "relations": [...], "count": N }`.

---

### `PATCH /v1/memory/relations/:id`

Promote a proposed relation into the curated graph. Tenant auth. Relations are otherwise immutable (no `title`/`content` to edit) — this endpoint exists *solely* for proposal acceptance.

**Request body** — exactly:

```json
{ "status": "accepted" }
```

Any other body shape returns `400`. To reject a proposed relation, `DELETE` it.

**Both endpoints must already be accepted** before a relation can be accepted. If either endpoint is still `status="proposed"`, this returns:

```
409 {"error": "cannot accept relation — it must be in 'proposed' status and both endpoint entries must already be accepted.  Accept the entry endpoints first, then retry."}
```

So the canonical reviewer flow for a multi-row proposal (entry A + entry B + edge A→B) is: accept A, accept B, then accept the edge.

**Response 200:** the updated `Relation` with `status: "accepted"`.

---

### `DELETE /v1/memory/relations/:id`

Delete one edge. Tenant auth. Works on both accepted and proposed rows; the latter is the **rejection path for a proposed relation**.

**Response 200:** `{ "deleted": true }`. `404` if not found.

---

### `GET /v1/memory/graph`

Single bulk fetch — the frontend calls this on mount to hydrate the force graph in one round trip. Tenant auth.

**Query parameters:**

| Name   | Type       | Description                                                    |
|--------|------------|----------------------------------------------------------------|
| `type` | csv string | Filter entries to these types; relations are pruned to those whose endpoints both survive the filter, so the snapshot stays self-consistent. |

**Response 200:**

```json
{
  "tenant_id": 1,
  "entries":   [ ... ],
  "relations": [ ... ]
}
```

No pagination in v1 — the unfiltered set is expected to fit in one response. We can revisit when a tenant grows past the per-page entry limit; until then the entry sweep caps the snapshot at the page count's ceiling.

The graph endpoint returns the **curated graph only** — proposed entries and edges are hidden so the visualised graph never shows pending review items.

---

### `GET /v1/memory/proposals`

List the agent-proposed entries and relations awaiting human review for the authenticated tenant. Tenant auth.

**Query parameters** (all optional):

| Name    | Type   | Description                                                                  |
|---------|--------|------------------------------------------------------------------------------|
| `kind`  | string | `entries` or `relations` to scope to one collection. Default returns both.   |
| `limit` | int    | Per-collection cap. Defaults: 100 entries, 200 relations.                    |

**Response 200:**

```json
{
  "tenant_id": 1,
  "entries": [
    {
      "id": 88,
      "tenant_id": 1,
      "type": "project",
      "title": "Investigate cache stampede on /v1/orchestrate",
      "content": "",
      "source": "agent",
      "tags": [],
      "status": "proposed",
      "created_at": 1777060001,
      "updated_at": 1777060001
    }
  ],
  "entries_count": 1,
  "relations": [
    {
      "id": 12,
      "tenant_id": 1,
      "source_id": 88,
      "target_id": 42,
      "relation": "relates_to",
      "status": "proposed",
      "created_at": 1777060002
    }
  ],
  "relations_count": 1
}
```

When `?kind=entries` is set, `relations`/`relations_count` are omitted; vice versa for `?kind=relations`.

---

### Agent access to structured memory

Agents running inside `/v1/orchestrate` (or `/v1/conversations/:id/messages`) can read and propose into structured memory in real time during a turn. Reads see only the curated graph; writes go to the proposal queue.

| Command                                                | Effect                                                                        |
|--------------------------------------------------------|-------------------------------------------------------------------------------|
| `/mem entries`                                         | List the tenant's accepted entries (up to 100), newest first.                 |
| `/mem entries project,reference`                       | Same, filtered to one or more types.                                          |
| `/mem entry 42`                                        | Full content of one accepted entry plus its incoming and outgoing edges.     |
| `/mem search <query>`                                  | Substring match on title + content of accepted entries (up to 50 hits).       |
| `/mem propose entry <type> <title>`                    | Propose a new typed node. Lands in `status="proposed"` (hidden from reads).   |
| `/mem propose link <src_id> <relation> <dst_id>`       | Propose a directed edge between two entries (either or both may be proposed).|

Read output lands in a `[/mem entries]` / `[/mem entry]` / `[/mem search]` tool-result block in the agent's next turn, framed by `[END MEMORY]`. Propose output lands in a `[/mem propose entry …]` or `[/mem propose link …]` block — typically `OK: proposed entry #88 [project] …` (or `OK: proposed relation #12 …`) so the agent can reference the new id in the same turn (e.g., propose two entries, then propose a link between them).

**Why proposals can't be read back.** Agents see only the curated graph — even from their own slash commands. `/mem entry <id>` on a still-proposed id returns `ERR: entry <id> not found`. This is deliberate: it prevents one agent from priming itself (or another agent in the same fleet) on unreviewed content, and it prevents a tenant's pending review queue from leaking across orchestrator boundaries.

The reader and writer are both bound to the request's authenticated tenant — sub-agents invoked via `/agent` and parallel children spawned via `/parallel` inherit the same tenant scope. CLI/REPL contexts (`arbiter --send`, the interactive REPL) don't have a tenant; both `/mem entries…` and `/mem propose…` return ERR there. This surface is API-only.

---

### Proposal queue

When agents call `/mem propose entry` or `/mem propose link`, rows land in the database with `status="proposed"`:

- They are **invisible to every read path**: HTTP `GET /v1/memory/entries`, `GET /v1/memory/entries/:id`, `GET /v1/memory/relations`, `GET /v1/memory/graph`, and the agent's own `/mem entries|entry|search` all filter to `status="accepted"`.
- They surface only through `GET /v1/memory/proposals` (the reviewer UI surface).

**Reviewing a proposal — accept:**

- Entry: `PATCH /v1/memory/entries/:id` with body `{"status": "accepted"}`.
- Relation: `PATCH /v1/memory/relations/:id` with body `{"status": "accepted"}`. Both endpoint entries must already be accepted; if either is still proposed, the PATCH returns `409` and the reviewer accepts the entry endpoints first.

**Reviewing a proposal — reject:**

- Entry: `DELETE /v1/memory/entries/:id`. This also cascade-deletes any proposed (or accepted) relation pointing to or from this entry.
- Relation: `DELETE /v1/memory/relations/:id`.

There is no separate `/reject` verb — `DELETE` is the rejection path. Once an entry or relation has been accepted, the same endpoints continue to work for normal CRUD; the proposal-queue mechanics simply stop applying.

**End-to-end flow** for an agent proposing two new related entries:

1. Agent emits `/mem propose entry project Investigate cache stampede` → response includes `OK: proposed entry #88 ...`.
2. Agent emits `/mem propose entry reference RFC-0042` → `OK: proposed entry #89 ...`.
3. Agent emits `/mem propose link 88 relates_to 89` → `OK: proposed relation #12 ...`.
4. Reviewer hits `GET /v1/memory/proposals` and sees both entries + the relation.
5. Reviewer accepts entries first: `PATCH /v1/memory/entries/88 {"status":"accepted"}`, same for `89`.
6. Reviewer accepts the edge: `PATCH /v1/memory/relations/12 {"status":"accepted"}`.
7. The next agent turn sees `#88`, `#89`, and the edge between them in `/mem entries` and `/mem entry 88`.

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

## MCP servers

Arbiter ships a pluggable [Model Context Protocol](https://modelcontextprotocol.io) client. Operators register external MCP servers — Microsoft's playwright server for browsing, Stripe's MCP for billing, anything that speaks the protocol — and agents reference them mid-turn through `/mcp` slash commands.

The integration is **per-request and stateful within a request**:

- Each `/v1/orchestrate` call (and each `POST /v1/conversations/:id/messages` call, and each `/v1/agents/:id/chat` call) gets its own `mcp::Manager` and its own subprocess.
- Within that request, multiple `/mcp` commands share the same browser context — `/mcp call playwright browser_navigate {...}` followed by `/mcp call playwright browser_snapshot` see the same tab.
- When the request ends (or the client cancels via `/v1/requests/:id/cancel`), the manager is destroyed and the subprocess receives `SIGTERM`. State does not persist across requests.

The cold-start cost of spawning a server lazily on first reference is real (`npx @playwright/mcp` is multi-second) but acceptable in exchange for clean tenant isolation and zero zombie-cleanup logic. The first `/mcp` reference per request pays the cost; subsequent ones in the same turn share the live process.

### Registry

Configure available servers via `~/.arbiter/mcp_servers.json` (path comes from `ApiServerOptions::mcp_servers_path` — defaults to `~/.arbiter/mcp_servers.json` when running `arbiter --api`):

```json
{
  "servers": {
    "playwright": {
      "command": "npx",
      "args": ["@playwright/mcp@latest"],
      "env": {
        "PLAYWRIGHT_BROWSERS_PATH": "/var/lib/arbiter/playwright"
      },
      "init_timeout_ms": 60000,
      "call_timeout_ms": 30000
    }
  }
}
```

| Field             | Type           | Required | Description                                                                                              |
|-------------------|----------------|----------|----------------------------------------------------------------------------------------------------------|
| `command`         | string         | yes      | Executable to run. PATH-resolved via `execvp`.                                                          |
| `args`            | array<string>  | no       | Arguments after `command`.                                                                              |
| `env`             | object         | no       | Extra `KEY: "VALUE"` strings appended to the parent environment.                                         |
| `init_timeout_ms` | int            | no       | Wall-clock budget for the JSON-RPC `initialize` handshake. Defaults to 60s — first-run `npx` may install. |
| `call_timeout_ms` | int            | no       | Per-`tools/call` timeout. Defaults to 30s. Playwright snapshots/navigation routinely take 5–15s.        |

Missing file = no MCP servers configured (clean ERR from `/mcp` rather than a startup failure). Malformed file throws at registry-load time so the operator sees the error in the API server log immediately. The arbiter binary itself does not install any server — operators bring their own (`npm install -g @playwright/mcp`, etc.).

### Slash commands

Agents drive the catalog via three subcommands. Both `/mcp tools` and `/mcp call` are gated to the request's `mcp::Manager` — agents in CLI/REPL contexts get `ERR: MCP unavailable` and adapt.

| Command                                                | Effect                                                                                  |
|--------------------------------------------------------|-----------------------------------------------------------------------------------------|
| `/mcp tools`                                           | List every configured server's tools (lazy-spawns each on first listing).               |
| `/mcp tools <server>`                                  | List one server's tools.                                                                |
| `/mcp call <server> <tool> [json_args]`                | Invoke a tool. `json_args` must be a JSON object if present.                            |

A typical playwright session inside one agent turn:

```
/mcp call playwright browser_navigate {"url":"https://news.ycombinator.com"}
/mcp call playwright browser_snapshot
/mcp call playwright browser_click {"ref":"link-42"}
/mcp call playwright browser_snapshot
```

Each call's response lands in a `[/mcp call …] … [END MCP]` tool-result block in the agent's next turn, framed identically to `/fetch` and `/exec`. Tool-call results are capped at 16 KB per call (matching `/fetch`); larger snapshots are truncated with `... [truncated]`. Agents that need full content should follow up with narrower queries.

### Output shape

`/mcp tools <server>` renders one tool per line with the first paragraph of its description (truncated at 120 chars). The server's full input schema is *not* inlined — agents that need it should read the corresponding MCP server's docs or call `tools/list` directly via `/mcp call`.

`/mcp call <server> <tool> {...}` concatenates `text` content items from the MCP `tools/call` response. Non-text content (images, embedded resources) is annotated as `[non-text content: <type> (<mimeType>) — agent surfaces only text]` so the agent knows something exists but isn't decoded inline. A future revision may surface playwright screenshots via SSE as separate artifacts.

### Billing

MCP calls are **not** billed — they don't consume tokens. The agent's tokens spent emitting and consuming the slash command are billed as normal LLM usage; the subprocess itself is server compute. Operators concerned about runaway cost should rate-limit at the proxy or cap concurrent requests per tenant.

### Failure modes

| Symptom                                          | Cause                                                                | Surface                                       |
|--------------------------------------------------|----------------------------------------------------------------------|-----------------------------------------------|
| `ERR: no MCP server '<name>' configured`         | Slash command names a server not in the registry.                    | Tool-result block; agent retries / abandons.  |
| `ERR: MCP server stopped responding during ...` | Subprocess crashed mid-call or `call_timeout_ms` elapsed.            | Tool-result block; subsequent calls re-spawn. |
| `ERR: invalid JSON args: ...`                    | `json_args` failed to parse or wasn't an object.                     | Tool-result block; agent re-emits with valid JSON. |
| `ERR: MCP unavailable in this context`           | CLI/REPL context — no `mcp::Manager` is wired (HTTP-only feature).   | Tool-result block; agent drops the /mcp step. |

A subprocess that crashes mid-request is **not auto-restarted** within that request — the manager keeps the dead `Client` and subsequent calls return ERR. The next request gets a fresh manager and a fresh subprocess. This avoids resurrection loops on a server that's broken for protocol reasons.

---

## Web search

Agents can issue `/search <query>` mid-turn to discover sources before fetching them — the missing piece that turned previous research turns into "guess a DOI from training memory and hope it resolves." Configured per-deployment via two `ApiServerOptions` fields:

| Field              | Default | Description                                                                                                |
|--------------------|---------|------------------------------------------------------------------------------------------------------------|
| `search_provider`  | `brave` | Search backend. Only `brave` is implemented in v1; `tavily` and `exa` slots reserved.                      |
| `search_api_key`   | `""`    | Provider API key. Empty ⇒ `/search` returns ERR with a clear "configure ARBITER_SEARCH_API_KEY" message.   |

`arbiter --api` reads the key from `ARBITER_SEARCH_API_KEY` (preferred — provider-agnostic name) or `BRAVE_SEARCH_API_KEY` (convenience for Brave-only deployments). The provider can be set via `ARBITER_SEARCH_PROVIDER`. Without a key the slash command degrades cleanly: agents see `ERR: web search unavailable in this context` and adapt by dropping the `/search` step.

### Slash commands

| Command                          | Effect                                                                              |
|----------------------------------|-------------------------------------------------------------------------------------|
| `/search <query>`                | Top 10 results for the query.                                                       |
| `/search <query> top=N`          | Top N results (clamped to 1..20). The `top=N` token is stripped from the query.      |

Capped at **4 searches per turn** (vs. /fetch's 3), since result bodies are small and agents typically need a couple of search→fetch round trips per topic.

### Result format

Numbered lines, one per hit, with title, snippet, and URL:

```
[/search planet nine 2024 top=5]
1. Title of the first result — Snippet text from the search engine, lightly trimmed.
   https://example.com/article-1
2. Second result title — More snippet text.
   https://example.com/article-2
...
[END SEARCH]
```

The dispatcher applies a 16 KB body cap (matching /fetch). Brave's `<strong>...</strong>` highlighting is stripped before rendering.

### Provider notes

**Brave** — `https://api.search.brave.com/res/v1/web/search`. The free tier gives 2,000 queries/month; production deployments should set a paid plan and a per-tenant rate limit at the proxy layer. Errors propagate verbatim: `ERR: Brave returned 401` on a bad key, `ERR: Brave rate-limited (429)` on quota exhaustion.

### Workflow for agents

The master constitution (and every research-flavoured starter agent) instructs agents to **search → fetch → browse**, in that order of escalation:

```
/search planet nine orbital clustering 2024
                                                       # next turn returns ranked URLs
/fetch https://arxiv.org/abs/2403.05451                # cheap libcurl read — preferred
/browse https://www.nature.com/articles/...            # JS / paywall — playwright
```

Guessing URLs from prior knowledge produces fabricated DOIs and dead links — `/search` discovers them, `/fetch` reads them when the page is static, and `/browse` falls back to a real browser when /fetch hits Cloudflare's "Just a moment", a login wall, or an SPA-only page.

### `/browse <url>`

JS-rendering fetch via the configured **playwright** MCP server. Composes two MCP calls under the hood:

1. `playwright/browser_navigate {"url": "..."}` — opens the URL.
2. `playwright/browser_snapshot` — captures the rendered accessibility tree.

The snapshot text is what arrives in the agent's tool-result block; the navigate confirmation is suppressed. On nav failure (timeout, transport ERR, or `isError=true`), the snapshot is skipped and the navigate error surfaces verbatim.

**Requirements:** an MCP server registered as `playwright` in `mcp_servers_path` (see [MCP servers](#mcp-servers) for registry shape and the sample at `docs/mcp_servers.example.json`). Without it, `/browse` returns:

```
ERR: /browse requires a playwright MCP server configured for this deployment.
Adapt: try /fetch <url> instead (works for static pages).
```

**Budget:** `/browse` and `/fetch` share a combined **3 URL reads per turn** — they're alternatives. Cold-start cost on the first `/browse` per request is multi-second (npx-spawning Chromium); subsequent `/browse` calls in the same turn share the live browser context per the [per-request, stateful MCP model](#mcp-servers).

**When to escalate to /browse:**

| Symptom from /fetch                              | Escalate? |
|--------------------------------------------------|-----------|
| Empty body or just nav chrome on a JS-heavy SPA  | yes       |
| "Just a moment..." (Cloudflare interstitial)     | yes       |
| Login-wall redirect, no article body             | yes       |
| Static HTML with the content present             | no — keep /fetch |

**Don't** /browse a URL that /fetch already retrieved successfully — that's a wasted browser spawn.

---

## Artifacts

Persistent server-side file storage scoped to **(tenant, conversation)**. Replaces the file-on-disk model with a sandboxed blob store that lives in the same SQLite database as conversations and structured memory — no host filesystem access, no path-traversal attack surface, automatic cleanup when a conversation is deleted.

The default `/write` slash command stays **ephemeral** — content is streamed as an SSE `file` event the frontend renders, with no server-side persistence. Adding `--persist` saves the same content into the artifact store. Two slash commands let agents read it back: `/read <path>` and `/list`.

### Storage model

| Property | Value |
|----------|-------|
| Backing store | SQLite BLOB column on `tenant_artifacts` |
| Primary key | `(tenant_id, conversation_id, path)` unique |
| Concurrency | Serialised by `SQLITE_OPEN_FULLMUTEX` (see Operational notes) |
| Cascade delete | FK `ON DELETE CASCADE` on `tenants(id)` and `conversations(id)` — drop a conversation, its artifacts go with it |

Paths are validated by a single canonical `sanitize_artifact_path` helper used by every entry point. Rules:

| Rejected | Reason |
|----------|--------|
| Empty path or > 256 chars | length |
| Component > 128 chars | length |
| Absolute (`/foo`, leading `\`) | path traversal protection |
| Drive letter (`C:\`, anything with `:`) | Windows safety |
| `..`, `.` | path traversal |
| Hidden (`.env`, `.git`, any `.foo`) | accidental dotfile leakage |
| Null bytes or control chars (< 0x20 or 0x7f) | injection / display safety |
| Windows-reserved names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-9, `LPT1`-9, case-insensitive, with or without extension) | cross-platform safety |

Backslashes are normalised to forward slashes before validation; repeated separators collapse; trailing slash is dropped. The canonical form is what goes into the unique index.

### Quotas

Hard ceilings, enforced inside `put_artifact` for every entry point:

| Scope | Default |
|-------|---------|
| Per file | **1 MB** |
| Per conversation | **50 MB** |
| Per tenant | **500 MB** |

PUT-on-conflict semantics: writing to an existing path **replaces** the row (same `id`, bumped `updated_at`), and quota math subtracts the existing size before checking the cap. Overwriting a 100 KB file with 200 KB only "costs" 100 KB against the conversation quota.

Responses surface the post-write totals in `tenant_used_bytes` and `conversation_used_bytes` so callers (and the agent's own tool result) know how close to the cap they are.

### `POST /v1/conversations/:id/artifacts`

Create or update an artifact in a conversation's working directory. Tenant auth + conversation existence check.

**Request body:**

| Field        | Type   | Required | Description                                                |
|--------------|--------|----------|------------------------------------------------------------|
| `path`       | string | yes      | Will be sanitized — caller can pass user-supplied paths.   |
| `content`    | string | yes      | UTF-8 string. Binary blobs over the JSON path is awkward; for binary content use a future direct-upload endpoint. |
| `mime_type`  | string | no       | Defaults to `application/octet-stream`. Free-form; not sniffed. |

**Responses:**
- `201 Created` on a fresh insert, `200 OK` on overwrite, body is `{ "artifact": {...}, "tenant_used_bytes": N, "conversation_used_bytes": N, "created": bool }`.
- `400` — invalid JSON or invalid path. The error message is the sanitizer's reason.
- `404` — conversation not found.
- `413` — quota exceeded (per-file, per-conversation, or per-tenant). Body identifies which.

```json
{
  "artifact": {
    "id": 12,
    "tenant_id": 1,
    "conversation_id": 7,
    "path": "output/report.md",
    "sha256": "ad14a...e3",
    "mime_type": "text/markdown",
    "size": 1832,
    "created_at": 1777060001,
    "updated_at": 1777060123
  },
  "tenant_used_bytes": 4231,
  "conversation_used_bytes": 1832,
  "created": false
}
```

### `GET /v1/conversations/:id/artifacts`

List the conversation's artifacts, newest-`updated_at` first. Capped at 200 per page (no cursor in v1; conversations of that size should migrate off the SQLite tier). Tenant auth.

**Response 200:** `{ "conversation_id": N, "artifacts": [...], "count": N, "bytes_used": N, "tenant_bytes_used": N }`. Each entry is the metadata shape above (no `content`).

### `GET /v1/conversations/:id/artifacts/:aid`

Metadata for one artifact. Returns the same shape as a list entry. `404` if the id doesn't exist for this tenant + conversation pair.

### `GET /v1/conversations/:id/artifacts/:aid/raw`

Raw content blob with proper `Content-Type` (from the artifact's `mime_type`) and a strong `ETag` (= `"<sha256>"`). Conditional `If-None-Match` returns `304 Not Modified` cheaply. Tenant auth.

### `DELETE /v1/conversations/:id/artifacts/:aid`

Permanently delete the artifact. Tenant auth. **Response 200:** `{ "deleted": true }`.

### `GET /v1/artifacts`

Tenant-wide cross-conversation discovery. Same response shape as the conversation list, minus the `conversation_id` field. Useful for a sibling UI rendering "all my files" rather than "files in this thread".

### `GET /v1/artifacts/:aid` and `GET /v1/artifacts/:aid/raw`

Tenant-scoped lookups by artifact id — the conversation id is inferred from the row. Same semantics as the conversation-scoped versions; cross-tenant ids surface as 404 (never as 403, to avoid id-existence side channels).

### `DELETE /v1/artifacts/:aid`

Tenant-scoped delete. Same as the conversation-scoped version.

### Agent slash commands

| Command                                | Effect                                                                                                  |
|----------------------------------------|---------------------------------------------------------------------------------------------------------|
| `/write <path>`                        | Ephemeral SSE `file` event only. The default; matches the prior behaviour exactly.                      |
| `/write --persist <path>`              | SSE event AND artifact-store row. Returns "OK: persisted N bytes (artifact #ID, K of LIMIT bytes used)" so the agent can self-throttle on quota. |
| `/read <path>`                         | Reads a previously persisted artifact in this conversation. ERR if path is invalid or not present.       |
| `/list`                                | Lists this conversation's artifacts, one per line: `<path>  (<size> bytes, mime=<type>, id=<id>)`.       |

The `--persist` write goes through the same path validator as the HTTP endpoint — the agent can't smuggle in `..` or absolute paths even if it tries. CLI/REPL contexts (no conversation, no tenant) leave the artifact callbacks null; `/write --persist` falls back to ephemeral with a `WARN: --persist requested but artifact store is unavailable` line, and `/read`/`/list` return ERR.

### Frontend safety

The path string lands on the client as a UTF-8 display field — it's untrusted. **The frontend must NOT pass it directly to `fs.writeFile` or any other path-sensitive API** without its own client-side sanitizer (same rules as the server's, plus your platform's specifics). If you let the user save the artifact to disk, build the destination path from the basename and a vetted directory of your choosing — never from the agent-supplied path.

### Non-goals (v1)

- No object-storage backend yet (S3/MinIO). The same interface fronts a future tier when tenants push past the SQLite ceiling.
- No artifact versioning beyond PUT-overwrites. `git`-style history is a separate feature.
- No public / cross-tenant sharing.
- No mime sniffing; agents declare or accept the default. Frontends should validate the type field they trust before rendering inline.

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

### Conversation

| Field            | Type    | Notes                                                                          |
|------------------|---------|--------------------------------------------------------------------------------|
| `id`             | integer | Stable per tenant.                                                             |
| `tenant_id`      | integer | FK into `tenants`. Always equals the caller's tenant.                          |
| `title`          | string  | Display title. May be empty.                                                   |
| `agent_id`       | string  | Which agent this thread talks to.                                              |
| `agent_def`      | object? | Snapshot of the inline agent definition (if the conversation was created with one). Absent for preloaded agents. |
| `created_at`     | integer | Epoch seconds.                                                                 |
| `updated_at`     | integer | Epoch seconds. Bumped on every message append.                                 |
| `message_count`  | integer | Total messages in the thread.                                                  |
| `archived`       | boolean | Hidden from default UI views; not deleted.                                     |

### ConversationMessage

| Field                | Type    | Notes                                                                |
|----------------------|---------|----------------------------------------------------------------------|
| `id`                 | integer | Append-only, ordered.                                                |
| `conversation_id`    | integer | FK into `conversations`.                                             |
| `role`               | string  | `"user"` or `"assistant"`.                                           |
| `content`            | string  | Full message text.                                                   |
| `input_tokens`       | integer | 0 for user messages; full request total for assistant messages.      |
| `output_tokens`      | integer | Same.                                                                |
| `billed_micro_cents` | integer | What the tenant was billed for the turn this message belongs to.     |
| `created_at`         | integer | Epoch seconds.                                                       |
| `request_id`         | string? | Correlates to the SSE stream + `usage_log` row that produced it.     |

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

### CORS

Every response includes permissive CORS headers (`Access-Control-Allow-Origin: *`, methods `GET, POST, PATCH, DELETE, OPTIONS`, headers `Authorization, Content-Type, Accept`). `OPTIONS` preflights short-circuit before auth and return `204` so a SPA on a different origin can hit the API in dev with no proxy.

Bearer auth carries in the `Authorization` header — no cookies — so credentials are never sent. To restrict origins in production, terminate at a reverse proxy and override `Access-Control-Allow-Origin` there, or extend `kCorsHeaders` in `src/api_server.cpp` to read an allowlist from `ARBITER_CORS_ORIGINS`.

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
