# Arbiter HTTP API

Arbiter exposes its multi-agent orchestrator as an HTTP + Server-Sent Events API. One `POST /v1/orchestrate` drives the full agentic loop — master agent turns, delegated and parallel sub-agent calls, tool invocations, generated files — and streams the whole thing back as SSE events. Tenant authentication and usage billing are built in; a separate admin surface lets an external billing/dashboard service read the ledger.

Start with `arbiter --api --port 8080`. The default bind is `127.0.0.1`; production deployments should put TLS termination, DDoS protection, and rate limiting in a reverse proxy (nginx, caddy, cloudflare) in front of the process.

Each endpoint page below uses the same template: **Function**, **Request**, **Response**, **Failure modes**, **See also**.

## Concepts

- [Tenants](concepts/tenants.md)
- [Authentication](concepts/authentication.md)
- [Billing](concepts/billing.md)
- [Fleet streaming](concepts/fleet-streaming.md)
- [SSE event catalog](concepts/sse-events.md)
- [Structured memory](concepts/structured-memory.md)
- [Artifacts](concepts/artifacts.md)
- [MCP servers](concepts/mcp.md)
- [Web search](concepts/search.md)
- [Data model](concepts/data-model.md)
- [Operational notes](concepts/operations.md)

## Endpoints

### Top-level
- [`GET /v1/health`](health.md)
- [`GET /v1/models`](models.md)
- [`POST /v1/orchestrate`](orchestrate.md)
- [`POST /v1/requests/:id/cancel`](requests-cancel.md)

### Agents
- [`GET /v1/agents`](agents/list.md)
- [`POST /v1/agents`](agents/create.md)
- [`GET /v1/agents/:id`](agents/get.md)
- [`PATCH /v1/agents/:id`](agents/patch.md)
- [`DELETE /v1/agents/:id`](agents/delete.md)
- [`POST /v1/agents/:id/chat`](agents/chat.md)

### Conversations
- [`POST /v1/conversations`](conversations/create.md)
- [`GET /v1/conversations`](conversations/list.md)
- [`GET /v1/conversations/:id`](conversations/get.md)
- [`PATCH /v1/conversations/:id`](conversations/patch.md)
- [`DELETE /v1/conversations/:id`](conversations/delete.md)
- [`GET /v1/conversations/:id/messages`](conversations/messages-list.md)
- [`POST /v1/conversations/:id/messages`](conversations/messages-post.md)

### Memory (file scratchpads)
- [`GET /v1/memory`](memory/list-scratchpads.md)
- [`GET /v1/memory/:agent_id`](memory/get-scratchpad.md)

### Memory (structured graph)
- [`POST /v1/memory/entries`](memory/entries/create.md)
- [`GET /v1/memory/entries`](memory/entries/list.md)
- [`GET /v1/memory/entries/:id`](memory/entries/get.md)
- [`PATCH /v1/memory/entries/:id`](memory/entries/patch.md)
- [`DELETE /v1/memory/entries/:id`](memory/entries/delete.md)
- [`POST /v1/memory/relations`](memory/relations/create.md)
- [`GET /v1/memory/relations`](memory/relations/list.md)
- [`DELETE /v1/memory/relations/:id`](memory/relations/delete.md)
- [`GET /v1/memory/graph`](memory/graph.md)

### Artifacts
- [`POST /v1/conversations/:id/artifacts`](artifacts/conversations-create.md)
- [`GET /v1/conversations/:id/artifacts`](artifacts/conversations-list.md)
- [`GET /v1/conversations/:id/artifacts/:aid`](artifacts/conversations-get.md)
- [`GET /v1/conversations/:id/artifacts/:aid/raw`](artifacts/conversations-raw.md)
- [`DELETE /v1/conversations/:id/artifacts/:aid`](artifacts/conversations-delete.md)
- [`GET /v1/artifacts`](artifacts/list.md)
- [`GET /v1/artifacts/:aid`](artifacts/get.md)
- [`GET /v1/artifacts/:aid/raw`](artifacts/raw.md)
- [`DELETE /v1/artifacts/:aid`](artifacts/delete.md)

### Admin
- [`GET /v1/admin/tenants`](admin/tenants-list.md)
- [`POST /v1/admin/tenants`](admin/tenants-create.md)
- [`GET /v1/admin/tenants/:id`](admin/tenants-get.md)
- [`PATCH /v1/admin/tenants/:id`](admin/tenants-patch.md)
- [`GET /v1/admin/usage`](admin/usage.md)
- [`GET /v1/admin/usage/summary`](admin/usage-summary.md)

## Versioning

All routes are prefixed `/v1/`. Breaking changes will land at `/v2/`; additive changes (new fields on responses, new optional query params) ship under `/v1/` with a note in this index.
