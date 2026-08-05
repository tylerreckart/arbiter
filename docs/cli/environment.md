# Environment

Every environment variable arbiter reads, what it controls, and what overrides what.

## Provider keys

Arbiter routes hosted model calls through OpenRouter. Local model calls continue
to use Ollama when the model id starts with `ollama/`.

| Variable                | Used by                              | Fallback                          |
|-------------------------|--------------------------------------|-----------------------------------|
| `OPENROUTER_API_KEY`    | Hosted models such as `openai/...`, `anthropic/...`, `google/...` | `~/.arbiter/openrouter_api_key` file |
| `OLLAMA_HOST`           | Any agent whose `model` resolves to Ollama | `http://localhost:11434`     |
| `ARBITER_OPENROUTER_REFERER` | Optional `HTTP-Referer` attribution header | Arbiter GitHub URL |
| `ARBITER_OPENROUTER_TITLE` | Optional `X-OpenRouter-Title` attribution header | `Arbiter` |

Env-var values take precedence over the file values. The file is read once at process start; changes during a long-running `--api` session require a restart.

## Server (`--api`) configuration

| Variable                  | Purpose                                                                       |
|---------------------------|-------------------------------------------------------------------------------|
| `ARBITER_API_VERBOSE`     | When set to a non-empty, non-`0` value, mirrors every SSE event to stderr. Equivalent to passing `--verbose`. The CLI flag wins if both are present. |
| `ARBITER_DRAIN_SECONDS`   | Wall-clock grace period on `SIGTERM` / `SIGINT` shutdown. The listen socket closes immediately and every in-flight orchestration is signalled to cancel; the server then waits up to this many seconds for connection threads to finish before tearing down sandbox containers. `0` skips the wait. Default `30`. See [Operations → Graceful shutdown](../concepts/operations.md#graceful-shutdown). |
| `ARBITER_LOG_FORMAT`      | Output format for operational stderr events (startup, recovery sweep, drain, sandbox lifecycle). `human` (default) renders `[HH:MM:SS] [level] event key=value`. `json` emits one JSON object per line for log aggregators. The per-request SSE-mirror verbose mode keeps its existing human format regardless. See [Operations → Structured logging](../concepts/operations.md#structured-logging). |
| `ARBITER_PUBLIC_BASE_URL` | Public origin (scheme + host, no trailing slash) advertised in A2A agent cards. Set this when TLS terminates at a reverse proxy so cards use `https://…` instead of falling back to `http://` + the inbound `Host` header. Example: `https://arbiter.example.com`. |
| `ARBITER_ADMIN_TOKEN`     | Admin bearer for `/v1/admin/*`. Overrides `~/.arbiter/admin_token`. Prefer this in systemd `EnvironmentFile=` so the token is not only a file under HOME. |
| `ARBITER_TENANT_MAX_CONCURRENT` | Max concurrent in-flight LLM requests per tenant (`0` = unlimited). See [Operations → rate limiting](../concepts/operations.md#per-tenant-rate--concurrency-limiting). |
| `ARBITER_TENANT_RATE_PER_MIN`   | Token-bucket refill rate per tenant (`0` = unlimited). |
| `ARBITER_TENANT_RATE_BURST`     | Token-bucket capacity (defaults to `rate_per_min` when unset/`0`). |

## TUI session durability

| Variable                         | Purpose                                                                 | Default |
|----------------------------------|-------------------------------------------------------------------------|---------|
| `ARBITER_AUTOSAVE_INTERVAL_SEC`  | Periodic dirty flush for conversation files. `0` disables the timer (post-turn and mid-turn `save_async` still run). | `30` |
| `ARBITER_COMPACT_THRESHOLD`      | Fraction of the model context window (0–1) that triggers auto-compaction. | `0.75` |
| `ARBITER_COMPACT_DISABLED`       | When set to a non-empty, non-`0` value, disables automatic compaction. `/compact` still works. | unset |

See [`docs/tui/sessions.md`](../tui/sessions.md).

## Host `/exec` (TUI / `--send`)

| Variable / flag              | Purpose |
|------------------------------|---------|
| `--no-exec`                  | Disable host `/exec` in the TUI (default is **enabled** with a confirm gate). |
| `--allow-host-exec`          | Allow host `/exec` when running `arbiter --api` (unsafe; prefer the Docker sandbox). |
| `ARBITER_ALLOW_HOST_EXEC`    | Same as `--allow-host-exec` when set to a non-empty, non-`0` value. |

The API server keeps `/exec` **disabled** unless the per-tenant Docker sandbox is configured (below) or host exec is explicitly allowed.

## Per-tenant sandbox

Arbiter's `/exec` writ is disabled by default in the API. Setting `ARBITER_SANDBOX_IMAGE` enables a per-tenant Docker sandbox that confines `/exec` to a workspace volume shared with `/write` and `/read`. The idle reaper (`ARBITER_SANDBOX_IDLE_SECONDS`) is implemented. There is **no** env var for the workspaces root path yet (`~/.arbiter/workspaces/`). The full walkthrough is in [`docs/concepts/sandbox.md`](../concepts/sandbox.md); the env-var surface:

| Variable                                | Purpose                                                                                                | Default        |
|-----------------------------------------|--------------------------------------------------------------------------------------------------------|----------------|
| `ARBITER_SANDBOX_IMAGE`                 | Container image to run inside. Required — without this the sandbox stays off and `/exec` returns `ERR`. | unset          |
| `ARBITER_SANDBOX_RUNTIME`               | Runtime binary. v1 supports `docker` only.                                                             | `docker`       |
| `ARBITER_SANDBOX_NETWORK`               | Docker `--network` value. `none` keeps `/exec` offline; `bridge` lets it reach the internet.           | `none`         |
| `ARBITER_SANDBOX_MEMORY_MB`             | Hard memory cap per container, MB. `0` = no cap.                                                       | `512`          |
| `ARBITER_SANDBOX_CPUS`                  | CPU shares per container. `0` = no cap.                                                                | `1.0`          |
| `ARBITER_SANDBOX_PIDS_LIMIT`            | Max processes per container. `0` = no cap.                                                             | `256`          |
| `ARBITER_SANDBOX_EXEC_TIMEOUT`          | Wall-clock kill, seconds, per `/exec` call. `0` = no parent-side timeout.                              | `30`           |
| `ARBITER_SANDBOX_WORKSPACE_MAX_BYTES`   | Per-tenant workspace disk quota, bytes. `/write` over the cap returns ERR; reads still work. `0` = no quota. | `1073741824` (1 GiB) |
| `ARBITER_SANDBOX_IDLE_SECONDS`          | Idle threshold before a tenant container is stopped by the background reaper. `0` = no reaping.        | `1800` (30 min) |

A misconfigured sandbox (docker missing, image string empty, workspaces root unwritable) logs the reason at startup and keeps the server running with `/exec` disabled — the safe default for an exposed API server. Tenant workspaces land at `~/.arbiter/workspaces/t<tenant_id>/`.

## Web search

Arbiter agents can emit `/search <query>`. To make that route somewhere, configure a provider:

| Variable                       | Purpose                                                      |
|--------------------------------|--------------------------------------------------------------|
| `ARBITER_SEARCH_PROVIDER`      | Provider id. Currently `brave` (Brave Search API) is the implemented provider. Default: unset (search disabled). |
| `ARBITER_SEARCH_API_KEY`       | API key for the configured provider. Preferred — explicitly scoped to arbiter's search use. |
| `BRAVE_SEARCH_API_KEY`         | Convenience fallback when `ARBITER_SEARCH_API_KEY` is unset. Useful if you already have this var set for other tools. |

File fallback: `~/.arbiter/search_api_key` (written by `arbiter --setup-tools`). Precedence is env vars first, then the file. Without a key configured, `/search` returns `ERR` and the agent falls back to `/fetch` on URLs it already knows.

## Precedence summary

For each setting, the order arbiter checks (first hit wins):

1. **CLI flag** — `--port`, `--bind`, `--verbose`.
2. **`ARBITER_*` env var** — preferred for arbiter-specific config.
3. **Convention env var** — `BRAVE_SEARCH_API_KEY`, `OPENROUTER_API_KEY`, etc.
4. **`~/.arbiter/<file>`** — convenient for keys, less convenient for runtime config.
5. **Hard-coded default** — `127.0.0.1`, `8080`, `localhost:11434`.

## Files under `~/.arbiter/`

Distinct from env vars but listed here for completeness, since the env-vs-file precedence question is the most common operational confusion:

| Path                       | Purpose                                                              |
|----------------------------|----------------------------------------------------------------------|
| `openrouter_api_key`       | OpenRouter API key (one line, no whitespace).                        |
| `search_api_key`           | Brave Search API key for `/search` (one line). Written by `--setup-tools`. |
| `admin_token`              | Admin token used by `/v1/admin/*`. Generated automatically on first `--api` launch if missing. |
| `tenants.db`               | Tenant store (SQLite): identities, conversations (API + TUI sessions), scratchpads, structured memory, schedules, todos, lessons. Used by TUI and `--api`. |
| `agents/*.json`            | Agent constitutions.                                                 |
| `conversations/`           | Legacy TUI JSON archive (imported once into `tenants.db`) plus `layout.json` mirror. |
| `sessions/*.json`          | Legacy per-cwd snapshots (imported into the conversation store on earlier upgrades). |
| `memory/t<tid>/`           | Legacy filesystem scratchpad fallback; DB `agent_scratchpad` is primary. |
| `workspaces/t<id>/…`       | Per-tenant sandbox workspace (mode 0700). Created on demand when the sandbox is enabled. See [`docs/concepts/sandbox.md`](../concepts/sandbox.md). |
| `mcp_servers.json`         | Optional MCP server registry. See [`docs/concepts/mcp.md`](../concepts/mcp.md). Editable via `arbiter --setup-tools`. |
| `history`                  | Merged TUI editor history across panes.                              |

Files are read on demand by the relevant subsystem. None of them are watched for changes — restart the process to pick up edits to `agents/*.json` or `mcp_servers.json` while `--api` is running.

## Notes

- `~/.arbiter/` is resolved from `$HOME` (or `getpwuid()` if `$HOME` is unset). `XDG_CONFIG_HOME` is not honoured. To run isolated arbiter instances, override `HOME` for the process: `HOME=/some/other/dir arbiter`.
- No env var controls log level beyond the verbose flag — verbose is binary, on or off.

Anything else arbiter reads is implicit (system clock, locale, timezone) and not configurable.
