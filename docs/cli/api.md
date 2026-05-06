# `arbiter --api`

Run arbiter as an HTTP+SSE orchestration server. Long-running, multi-tenant, bearer-token authenticated. The on-the-wire protocol is documented in [`docs/api/`](../api/index.md); this page covers the CLI flags and operational behaviour.

```
arbiter --api [--port N] [--bind ADDR] [--verbose]
```

| Flag        | Default       | What it does                                                |
|-------------|---------------|-------------------------------------------------------------|
| `--port`    | `8080`        | TCP port to listen on.                                      |
| `--bind`    | `127.0.0.1`   | Bind address. `0.0.0.0` to accept remote traffic.           |
| `--verbose` | off           | Mirror every SSE event (text deltas, tool calls, thinking) to stderr. Equivalent to `ARBITER_API_VERBOSE=1`. Verbose flag wins over the env var. |

## What it does on startup

1. Opens `~/.arbiter/tenants.db`. Warns if no tenants exist (server still starts; every `/v1/orchestrate` returns 401 until you `--add-tenant`).
2. Reads provider API keys from env or `~/.arbiter/`. See [environment.md](environment.md).
3. Resolves the admin token used to authorise tenant-management endpoints (`/v1/admin/*`). See `~/.arbiter/admin_token` — created on first `--api` launch if missing.
4. Loads agent constitutions from `~/.arbiter/agents/*.json`.
5. Loads the optional MCP registry from `~/.arbiter/mcp_servers.json`. See [`docs/api/concepts/mcp.md`](../api/concepts/mcp.md).
6. Loads the optional A2A remote-agent registry from `~/.arbiter/a2a_agents.json`. See [a2a-agents.md](a2a-agents.md).
7. Reads optional web-search provider config from `ARBITER_SEARCH_PROVIDER` / `ARBITER_SEARCH_API_KEY` (or `BRAVE_SEARCH_API_KEY` as a convenience fallback).
8. Reads optional billing-service URL from `ARBITER_BILLING_URL`. Unset = no eligibility checks, no usage record posting; provider keys go through directly with no caps.
9. Clears the terminal (ANSI `\033[2J\033[3J\033[H`) and prints the banner + endpoint summary at row 1, so the bind address and admin-token lines anchor at the top of a clean screen instead of chasing prior shell history.
10. Binds the listen socket and starts accepting requests.

`SIGINT` / `SIGTERM` triggers graceful shutdown — in-flight SSE streams close cleanly, the listen socket closes, the process exits.

## What it doesn't do

- **TLS termination.** The server speaks plain HTTP. Production deployments should put TLS termination, DDoS protection, and rate limiting in a reverse proxy (nginx, caddy, cloudflare) in front of the process.
- **Sandboxing.** When configured for SaaS use the API server disables `/exec` (no shell execution from agents) and intercepts `/write` so file output streams to the client instead of landing on the server's disk. This is a reasonable default but is not a substitute for OS-level isolation — run the process in a container or a dedicated user account.
- **Process supervision.** The binary doesn't daemonise itself. Run it under systemd / launchd / docker / pm2 — whichever your platform uses.

## Endpoint reference

The HTTP surface is documented in detail in [`docs/api/`](../api/index.md). Quick orientation:

- **`POST /v1/orchestrate`** — drives the full agentic loop, streams everything back as SSE.
- **`GET /v1/agents`**, **`POST /v1/agents`**, etc. — manage the tenant-stored agent catalogue.
- **`POST /v1/conversations`**, **`/messages`** — long-running conversations with persisted history.
- **`/v1/memory/*`** — structured memory graph (typed nodes, relations, FTS search).
- **`/v1/artifacts/*`** — files agents wrote.
- **`/v1/a2a/agents/:id`** — Agent2Agent (A2A) protocol surface: per-agent cards plus JSON-RPC dispatch (`message/send`, `message/stream`, `tasks/get`, `tasks/cancel`). See [`docs/api/concepts/a2a.md`](../api/concepts/a2a.md).
- **`/v1/admin/tenants/*`** — tenant lifecycle (admin-token gated).

## Tenant identity

Every authenticated request carries `Authorization: Bearer <token>` where the token was issued by `arbiter --add-tenant <name>`. Tokens are stored hashed (SHA-256) in `~/.arbiter/tenants.db`; the plaintext is shown exactly once at provisioning time. See [tenants.md](tenants.md).

## Billing

Optional. When `ARBITER_BILLING_URL` is set, the runtime exchanges every bearer for a workspace_id via `POST /v1/runtime/auth/validate`, pre-flights against `POST /v1/runtime/quota/check`, and posts post-turn telemetry to `POST /v1/runtime/usage/record`. With the env var unset, the runtime acts as a thin pass-through using the operator-supplied provider keys, with no eligibility checks. Operators wanting a commercial deployment must implement the runtime's billing protocol against a service of their own choosing — arbiter ships no reference implementation in this repository.

## Logging

By default the server logs only structured events (request received, request completed, errors). Add `--verbose` (or `ARBITER_API_VERBOSE=1`) to mirror SSE events to stderr — useful when debugging an agent's tool use, noisy under normal load. Verbose mode renders one stderr line per event, colour-coded:

- text deltas (line-buffered, flushed on newline)
- `tool_call` — slash-command execution with ✓/✗ status
- `advisor` — runtime advisor activity: `advise` (executor `/advise` consult), `gate ✓` (continue), `gate ↻` (redirect with guidance), `gate ✗` (halt with reason), `gate ⛔` (redirect budget exhausted). See [`docs/api/concepts/sse-events.md`](../api/concepts/sse-events.md) and [`docs/api/concepts/advisor.md`](../api/concepts/advisor.md).
- `escalation` — out-of-band advisor halt notification (sibling of `stream_end`)
- `file` — `/write` writes streamed to the client

Logs go to stderr; redirect with `2>` in your service unit if you want them captured separately from stdout.

## Resource expectations

- Each in-flight `/v1/orchestrate` request runs its own Orchestrator instance with a per-request MCP manager. No global mutex on the request path; per-request state is isolated.
- SQLite is the storage layer (`~/.arbiter/tenants.db`). Single-writer; safe for one process. If you need to scale to multiple processes, you'll need a different storage strategy — the schema is portable but the connection layer is currently tied to local SQLite.
- Memory: scales with concurrent in-flight requests + number of distinct tenants in active use. A few hundred MB is typical for low-traffic deployments; tail latencies are dominated by upstream provider response time, not by the server itself.

## Health check

```bash
curl http://127.0.0.1:8080/v1/health
```

Returns `{"ok": true}` once the server has finished startup. Useful as a Kubernetes liveness probe.

## See also

- [`docs/api/index.md`](../api/index.md) — endpoint reference.
- [`docs/api/concepts/authentication.md`](../api/concepts/authentication.md) — bearer-token semantics.
- [`docs/api/concepts/operations.md`](../api/concepts/operations.md) — operational notes.
- [tenants.md](tenants.md) — provisioning the bearer tokens this server validates.
