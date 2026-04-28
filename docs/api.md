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
  - [`GET /v1/agents/:id`](#get-v1agentsid)
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
    - [`DELETE /v1/memory/relations/:id`](#delete-v1memoryrelationsid)
    - [`GET /v1/memory/graph`](#get-v1memorygraph)
    - [Agent access](#agent-access-to-structured-memory)
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
| `agent_def`  | object  | no       | —         | Inline agent definition (same shape as in chat requests). Snapshotted onto the conversation so the thread is self-contained. |

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
| `agent_def`  | object   | no       | Override the conversation's agent for this one turn (rare). |

**What happens server-side:**

1. Conversation lookup — `404` if missing or wrong tenant. Validation surfaces as a clean JSON error before the SSE stream opens.
2. Prior messages loaded from the DB and replayed into the agent's history (capped at the most recent 100 turns to keep request payload bounded).
3. The user's `message` is persisted with the `request_id` issued for this stream.
4. The orchestrator runs and streams events exactly as `/v1/orchestrate` would.
5. On a successful `done`, the assistant's full response is persisted alongside the request's billing totals (`input_tokens`, `output_tokens`, `billed_micro_cents`).
6. On failure (`done.ok = false`), the assistant message is not persisted — only the user message remains. Retry is safe.

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

The two sub-systems do **not** share storage. An entry is not a parsed agent scratchpad; an agent's `/mem write` does not create entries. Conversely, agents *can* read structured memory in real time during a turn (see [Agent access](#agent-access-to-structured-memory) below) but cannot write it — writes happen only over HTTP.

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

**Response 201:** the new `Entry`.

```json
{
  "id": 42,
  "tenant_id": 1,
  "type": "project",
  "title": "Frontend graph",
  "content": "Force-directed view of memory.",
  "source": "planning",
  "tags": ["scope", "hub"],
  "created_at": 1777058449,
  "updated_at": 1777058449
}
```

---

### `GET /v1/memory/entries`

List entries for the authenticated tenant, newest `updated_at` first. Tenant auth.

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

Read one entry. Tenant auth. `404` if the id doesn't exist or belongs to another tenant.

---

### `PATCH /v1/memory/entries/:id`

Update any subset of `{title, content, source, tags, type}`. Tenant auth. `created_at` is immutable; `updated_at` is bumped on a successful change. `404` if not found.

**Response 200:** the updated `Entry`.

---

### `DELETE /v1/memory/entries/:id`

Permanently delete an entry. **Cascades to relations** with this entry as either endpoint. Tenant auth.

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

**Response 201:** the new `Relation`.

```json
{
  "id": 7,
  "tenant_id": 1,
  "source_id": 42,
  "target_id": 43,
  "relation": "supports",
  "created_at": 1777058500
}
```

**409** with `{"error": "relation already exists", "existing_id": N}` on a duplicate `(source, target, relation)` triple.

---

### `GET /v1/memory/relations`

List relations for the authenticated tenant. Tenant auth.

**Query parameters** (all optional):

| Name        | Type   | Description                       |
|-------------|--------|-----------------------------------|
| `source_id` | int    | Filter to edges from this entry.  |
| `target_id` | int    | Filter to edges to this entry.    |
| `relation`  | string | Filter to one relation kind.      |
| `limit`     | int    | Default 200, hard max 1000.       |

**Response 200:** `{ "relations": [...], "count": N }`.

---

### `DELETE /v1/memory/relations/:id`

Delete one edge. Tenant auth. **Response 200:** `{ "deleted": true }`. `404` if not found.

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

---

### Agent access to structured memory

Agents running inside `/v1/orchestrate` (or `/v1/conversations/:id/messages`) can read structured memory in real time during a turn via three slash sub-commands. Reads only — there is no slash path for write/update/delete; structured memory is HTTP-write-only.

| Command                                    | Effect                                                         |
|--------------------------------------------|----------------------------------------------------------------|
| `/mem entries`                             | List the tenant's entries (up to 100), newest first.           |
| `/mem entries project,reference`           | Same, filtered to one or more types.                           |
| `/mem entry 42`                            | Full content of one entry plus its incoming and outgoing edges.|
| `/mem search <query>`                      | Substring match on title + content (up to 50 hits).            |

Output lands in a `[/mem entries]` (or `[/mem entry]` / `[/mem search]`) tool-result block in the agent's next turn, framed by `[END MEMORY]`. The reader is bound to the request's authenticated tenant — sub-agents invoked via `/agent` and parallel children spawned via `/parallel` inherit the same tenant scope. CLI/REPL contexts (`arbiter --send`, the interactive REPL) don't have a tenant and the slash commands return ERR there; this is API-only.

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
