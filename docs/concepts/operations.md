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

## File cap

Agent-generated files (via `/write`, ephemeral path) are captured in memory and forwarded as `file` SSE events. A per-response cap (default 10 MiB across all files in the same request) kicks in if an agent tries to flood the stream. Beyond the cap, `/write` attempts return an `ERR:` tool result and the file is dropped. The persistent path (`/write --persist`) has its own quota structure — see [Artifacts](artifacts.md).

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
