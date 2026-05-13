# Operational notes

## Config files

Everything arbiter persists lives under `~/.arbiter/`:

| Path                     | Purpose |
|--------------------------|---------|
| `api_key`                | Anthropic API key (mode 0600). Read if `$ANTHROPIC_API_KEY` is unset. |
| `openai_api_key`         | OpenAI API key (mode 0600). Read if `$OPENAI_API_KEY` is unset. |
| `gemini_api_key`         | Google Gemini API key (mode 0600). Read if `$GEMINI_API_KEY` is unset. |
| `admin_token`            | Admin bearer token (mode 0600). Read if `$ARBITER_ADMIN_TOKEN` is unset. |
| `tenants.db`             | SQLite ledger. WAL mode + `SQLITE_OPEN_FULLMUTEX` (serialised threading), foreign keys enforced. Schema migrates on open. |
| `agents/*.json`          | Local example agent constitutions (CLI-mode only — the API path doesn't read these). |
| `memory/t<tenant_id>/*.md` | Legacy per-tenant agent file scratchpads. The agent file scratchpad now also has a DB-backed implementation (see structured memory + artifacts). |
| `workspaces/t<tenant_id>/` | Per-tenant sandbox workspace (mode 0700). Created on demand when the sandbox is enabled. See [Sandbox](sandbox.md). |
| `master_model`           | Override for the master agent's model. |
| `mcp_servers.json`       | Optional MCP server registry (see [MCP servers](mcp.md)). |

## SQLite concurrency

The ledger DB is opened with `SQLITE_OPEN_FULLMUTEX`, which forces SQLite into serialized threading mode regardless of the underlying library's build defaults. This matters because:

- The API server's accept loop spawns one thread per connection.
- Without serialization, sharing a single `sqlite3*` across threads is undefined behaviour and surfaces as either garbage error messages ("`no such table: <…>`") or `SIGSEGV` in `sqlite3RunParser`.
- `SQLITE_OPEN_FULLMUTEX` adds a per-connection mutex; the cost is invisible at our scale (hundreds of req/min ceiling on this tier).

If you build against a SQLite that already defaults to serialized mode (Linux distros typically do; macOS's system SQLite typically does not), the flag is a no-op.

## CORS

Every response includes permissive CORS headers (`Access-Control-Allow-Origin: *`, methods `GET, POST, PATCH, DELETE, OPTIONS`, headers `Authorization, Content-Type, Accept`). `OPTIONS` preflights short-circuit before auth and return `204` so a SPA on a different origin can hit the API in dev with no proxy.

Bearer auth carries in the `Authorization` header — no cookies — so credentials are never sent. To restrict origins in production, terminate at a reverse proxy and override `Access-Control-Allow-Origin` there, or extend `kCorsHeaders` in `src/api_server.cpp` to read an allowlist from `ARBITER_CORS_ORIGINS`.

## Per-tenant rate / concurrency limiting

The runtime keeps a per-tenant in-flight counter and a token-bucket rate limiter in front of the expensive routes (`POST /v1/orchestrate`, `POST /v1/conversations/:id/messages`, `POST /v1/agents/:id/chat`, `POST /v1/a2a/agents/:id`). Cheap reads (health, GET memory, GET schedules) are unaffected — the bucket isn't consumed.

| Env var                          | Meaning                                                            | Default        |
|----------------------------------|--------------------------------------------------------------------|----------------|
| `ARBITER_TENANT_MAX_CONCURRENT`  | Max concurrent in-flight LLM requests per tenant.                   | `0` (unlimited) |
| `ARBITER_TENANT_RATE_PER_MIN`    | Token-bucket refill rate per tenant.                                | `0` (unlimited) |
| `ARBITER_TENANT_RATE_BURST`      | Token-bucket capacity (max burst above refill rate).                | `rate_per_min` |

A `0` on either axis disables that axis (the limiter grants every acquire without taking the lock; zero overhead for operators not using the surface). Surplus requests get `429 Too Many Requests` with `Retry-After` set to a best-guess wait. The body distinguishes the two failure modes:

```json
{ "error": "rate limit exceeded",
  "reason": "concurrent_request_limit",
  "retry_after_seconds": 1 }
```

`reason` is one of `concurrent_request_limit` or `rate_limit`; clients can branch on it. Defaults are catch-the-runaway-loop, not fairness — operators wanting precise per-tenant SLAs should set explicit values via env or per-tenant overrides on the admin tenant row.

## Deployment

Run behind a reverse proxy. TLS, rate limiting, and DDoS protection are out of scope for arbiter itself — it binds `127.0.0.1` by default specifically to encourage this layout.

### Example nginx location block

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

## Scaling characteristics

- **One thread per connection.** Arbiter doesn't pool; each `/v1/orchestrate` gets a fresh `Orchestrator` with fresh agent history. Sub-agents spawned by `/parallel` are additional threads within that request.
- **SQLite on the write path.** Fine for single-node deployments up to a few hundred req/min. Multi-node = swap for Postgres (schema ports cleanly).
- **Per-request MCP subprocesses.** Each request that invokes `/mcp` or `/browse` spawns its own subprocess, killed at request end. Cold starts cost real wall-clock; cache `npx`-installed packages on a tmpfs or a persistent volume for production.

## Crash diagnostics

`ApiServer::start()` installs a `SIGSEGV` / `SIGABRT` / `SIGBUS` / `SIGFPE` handler that prints a backtrace to stderr before re-raising. Combined with the connection-level try/catch (which catches `std::exception` and turns it into a 500 with the `what()`), runaway throws don't take down the daemon — they get logged with the request method + path and surface as a clean 500 to the client.

Per-handler `[memory] tenant=<id> entry.patch.* …` breadcrumbs are emitted along the entry mutation paths so the *last* line printed before a crash localises the failing step.

## Metrics

`GET /v1/metrics` exposes Prometheus-format counters and gauges covering request flow (per tenant + route), provider call health (per provider), sandbox container lifecycle, idempotency cache hits/misses, and rate-limit rejections. The endpoint is unauthenticated by design — restrict it at the reverse proxy if you don't want every client on the metrics network to scrape it.

See the [metrics endpoint reference](../api/metrics.md) for the full metric list and a sample Prometheus scrape config.

## Structured logging

Operational events (startup, recovery sweep, shutdown drain, sandbox enable/disable, sandbox idle reaping) are routed through a single logger that emits either human-readable or JSON lines on stderr. Switch with `ARBITER_LOG_FORMAT`:

| Value    | Output                                                                       |
|----------|------------------------------------------------------------------------------|
| `human`  | Default. `[HH:MM:SS] [level] event key=value key=value`.                     |
| `json`   | One JSON object per line: `{"ts":"...","level":"...","event":"...",...}`.    |

JSON mode is intended for log aggregators (Loki, ELK, Datadog) that index by field rather than parse free-form text. Per-request stderr logs from `--verbose` SSE mirroring keep their existing human format in v1 — only the operational-event stream is structured.

```json
{"ts":"2026-05-11T14:23:01.412Z","level":"info","event":"sandbox_enabled","image":"arbiter/sandbox:latest","network":"none","memory_mb":"512","cpus":"1.00","pids":"256","timeout_s":"30"}
{"ts":"2026-05-11T14:23:01.413Z","level":"info","event":"recovery_sweep","orphaned_count":"2","new_state":"failed"}
{"ts":"2026-05-11T18:42:55.808Z","level":"info","event":"drain_started","in_flight":"4","deadline_s":"30"}
{"ts":"2026-05-11T18:42:58.221Z","level":"info","event":"drain_complete"}
```

## Provider circuit breaker

A per-provider circuit breaker sits in front of the per-request retry loop. After **5 consecutive failures** (5xx or 429 past the retry budget) against the same provider, the breaker opens for a **30 s cooldown**. Calls while open return a structured `circuit_open` error from the `done` event's `error_code` field — agents see a fast-fail instead of every parallel request burning four retries against a clearly-unhealthy upstream. When the cooldown elapses, the next call admits as a half-open probe; success closes the breaker, failure reopens it with a fresh cooldown.

State per provider is exposed via the `arbiter_provider_circuit_open_total` counter on `/v1/metrics`. Defaults are tuned conservatively for v1 — operator-tunable thresholds (env vars) are a Phase 5 follow-up.

## Admin audit log

Every mutation through `/v1/admin/*` (create tenant, disable tenant) writes an append-only row to the `admin_audit` table in `tenants.db`. Operators read the log via [`GET /v1/admin/audit`](../api/admin/audit.md). The runtime never edits or deletes audit rows; retention policy is the operator's call.

## SSE heartbeat

Every primary SSE response (`/v1/orchestrate`, `/v1/conversations/:id/messages`, `/v1/agents/:id/chat`, `/v1/requests/:id/events`) emits a `:` SSE comment frame every 30 seconds when the stream is otherwise idle. Comment frames are ignored by spec-compliant clients but keep TCP alive past reverse proxies that drop idle connections (nginx defaults to 60s, Cloudflare to 100s). Long-running LLM calls survive these limits without per-deployment tuning.

The heartbeat is implementation-detail: clients don't subscribe to or rely on it; resumability is handled by `since_seq` on the resubscribe path, not by counting heartbeats.

## Graceful shutdown

On `SIGINT` or `SIGTERM` the server:

1. Stops accepting new connections (closes the listen socket).
2. Issues `Orchestrator::cancel()` against every in-flight orchestration via the in-flight registry, so streaming LLM calls wake up and the SSE handler can write a final frame.
3. Waits up to `ARBITER_DRAIN_SECONDS` (default `30`) for in-flight connection threads to finish.
4. Tears down sandbox containers via `SandboxManager::stop_all()`.
5. Exits.

Connections still active past the drain deadline are abandoned with a stderr warning — the OS closes their sockets when the process exits. Set `ARBITER_DRAIN_SECONDS=0` to skip the wait entirely (useful for tests and tight CI shutdowns); set it higher than 30 if your longest expected LLM call is over half a minute.

## File cap

Agent-generated files (via `/write`, ephemeral path) are captured in memory and forwarded as `file` SSE events. A per-response cap (default 10 MiB across all files in the same request) kicks in if an agent tries to flood the stream. Beyond the cap, `/write` attempts return an `ERR:` tool result and the file is dropped. The persistent path (`/write --persist`) has its own quota structure — see [Artifacts](artifacts.md).

## Sandboxed `/exec`

By default the API server keeps `/exec` disabled and surfaces a clean `ERR:` to the agent. To enable shell execution for agents safely, configure the per-tenant Docker sandbox — see [Sandbox](sandbox.md) for setup, env vars, container lifecycle, resource caps, and failure modes. When the sandbox is wired, `/exec`, `/write`, and `/read` all share the tenant's `/workspace` bind mount so agents can build cross-turn workflows that produce and re-read files.

## Error response codes (cross-cutting)

| Code | Meaning |
|------|---------|
| 200  | Normal — for JSON admin responses, and the opening frame of an SSE orchestrate stream. |
| 201  | Resource created (POST endpoints). |
| 304  | `If-None-Match` matched a strong ETag (artifact `/raw` reads). |
| 400  | Malformed request (bad JSON, missing required field, bad tenant id, sanitiser rejection). |
| 401  | Missing / invalid / mismatched bearer token, or tenant `disabled=true`. |
| 404  | Unknown endpoint, or id not found / wrong tenant on a GET / PATCH / DELETE. |
| 405  | Method not allowed on an existing route. |
| 409  | Conflict (duplicate id). |
| 410  | The row vanished mid-request (concurrent DELETE). |
| 413  | Quota exceeded (artifact stores). |
| 500  | Uncaught exception during handling. The body includes `what()`. |
| 503  | Admin endpoint called while the server has no admin token configured. |

## See also

- [Authentication](authentication.md)
