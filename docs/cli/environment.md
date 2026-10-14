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

## Remote TUI (`--connect`)

| Variable             | Purpose |
|----------------------|---------|
| `ARBITER_API_URL`    | Default base URL for `arbiter --connect` when the flag omits a URL. Must be `http://` or `https://`. |
| `ARBITER_API_TOKEN`  | Default tenant bearer token for `--connect`. Preferred over `--token` (tokens on argv are visible in `ps`). |

See [`docs/cli/connect.md`](connect.md). Local provider keys (`OPENROUTER_API_KEY`, …) are **not** required on the client host for remote sessions.

## Server (`--api`) configuration

| Variable                  | Purpose                                                                       |
|---------------------------|-------------------------------------------------------------------------------|
| `ARBITER_API_VERBOSE`     | When set to a non-empty, non-`0` value, mirrors every SSE event to stderr. Equivalent to passing `--verbose`. The CLI flag wins if both are present. |
| `ARBITER_DRAIN_SECONDS`   | Wall-clock grace period on `SIGTERM` / `SIGINT` shutdown. The listen socket closes immediately and every in-flight orchestration is signalled to cancel; the server then waits up to this many seconds for connection threads to finish before tearing down sandbox containers. `0` skips the wait. Default `30`. See [Operations → Graceful shutdown](../concepts/operations.md#graceful-shutdown). |
| `ARBITER_LOG_FORMAT`      | Output format for operational stderr events (startup, recovery sweep, drain, sandbox lifecycle). `human` (default) renders `[HH:MM:SS] [level] event key=value`. `json` emits one JSON object per line for log aggregators. The per-request SSE-mirror verbose mode keeps its existing human format regardless. See [Operations → Structured logging](../concepts/operations.md#structured-logging). |
| `ARBITER_GLYPHS`          | State marks in the TUI and text lists. `unicode` (also `utf-8` / `shape`) is the shape set. `ascii` is the single-byte fallback. Unset or `auto` uses ASCII when `TERM` is `dumb` or the locale (`LC_ALL`, then `LC_CTYPE`, then `LANG`) is not UTF-8; otherwise unicode. |
| `ARBITER_CORS_ORIGINS`    | Comma-separated Origin allowlist. Unset ⇒ `Access-Control-Allow-Origin: *`. When set, only matching `Origin` values are echoed (with `Vary: Origin`); mismatches omit the header. See [Operations → CORS](../concepts/operations.md#cors). |
| `ARBITER_CIRCUIT_FAILURE_THRESHOLD` | Consecutive provider failures that open the circuit breaker. Default `5` (clamped ≥1). See [Operations → Circuit breaker](../concepts/operations.md#provider-circuit-breaker). |
| `ARBITER_CIRCUIT_COOLDOWN_SECONDS`  | Cooldown in Open before a half-open probe. Default `30` (clamped ≥0). |
| `ARBITER_INTENT_LLM_RATE_PER_MIN` | Extra per-tenant token bucket for `POST /v1/intent` hybrid/llm classify. Default `20`. `0` disables. Independent of `ARBITER_TENANT_RATE_PER_MIN`. See [Operations → Per-tenant rate / concurrency limiting](../concepts/operations.md#per-tenant-rate--concurrency-limiting). |
| `ARBITER_INTENT_LLM_RATE_BURST` | Burst for the intent LLM bucket. Default `5`. |
| `ARBITER_INTENT_SHADOW` | When `luna` (also `on` / `true` / `1`), depth-0 intent classification also asks `openai/gpt-6-luna` for a logprob distribution over the loaded roster and logs `intent_shadow`. When `embed`, the same log line is the local embedding router instead. Routing is unchanged. Unset or `off` disables it. Skipped when live mode is on, so the same backend is not called twice. |
| `ARBITER_INTENT_SHADOW_MODEL` | OpenRouter model id for the shadow call. Default `openai/gpt-6-luna`. |
| `ARBITER_INTENT_LIVE` | When `luna` (also `on` / `true` / `1`), depth-0 ingress uses the local router instead of the generative `<intent>` classifier. Default off. A cached Luna decision is the route. Otherwise the embedding model decides at every confidence, and a missing model uses `ARBITER_INTENT_DEFAULT_AGENT`. Luna runs in the background for a low-confidence embed or a missing model, and its result fills the cache for the next similar request. |
| `ARBITER_INTENT_LUNA_MIN_CONFIDENCE` | Luna confidence at or above this value is source `luna` when a cached result is applied, and it is the bar for `ARBITER_INTENT_LUNA_REROUTE`. Below it a cached result is source `luna_guess`. Default `0.55`. Clamped to [0, 1]. |
| `ARBITER_INTENT_LUNA_TIMEOUT_MS` | Budget for the background Luna call. Default `2000`, clamped to ≥ 1. Routing returns before this elapses. The default is sized to live OpenRouter measurements of `openai/gpt-6-luna` with reasoning off: about 1.0–1.4 s at p50 and 1.6–1.9 s at p95. A 2026-10-13 smoke over 4 calls was p50 1359 ms and p95 1564 ms. On expiry the call is cancelled and left out of the cache. The synchronous route is already the embedding choice, or the default agent when the embedding model is unavailable. |
| `ARBITER_INTENT_LUNA_REROUTE` | When `1` / `on` / `true` / `yes`, a Luna result that finishes before the turn's first model call may replace the specialist if it disagrees and its confidence is at least `ARBITER_INTENT_LUNA_MIN_CONFIDENCE`. The executor model stays the synchronous choice. Default off. |
| `ARBITER_INTENT_STRONG_DIFFICULTY` | Difficulty at or above this selects the strong executor model. Default `0.5`. |
| `ARBITER_INTENT_DEFAULT_DIFFICULTY` | Difficulty used when a successful Luna call has no cheap/strong labels. Default `0.5`. Heuristic and fallback turns keep the agent's own model. |
| `ARBITER_INTENT_CACHE_TTL_MS` | LRU TTL for a normalized request plus roster. Default `60000`. `0` disables the cache. |
| `ARBITER_INTENT_CACHE_ENTRIES` | LRU capacity. Default `64` (clamped ≥ 1). |
| `ARBITER_INTENT_CACHE_KEY_CHARS` | Normalized request length kept in the cache key. Default `512`. |
| `ARBITER_INTENT_DEFAULT_AGENT` | Synchronous route when the embedding model is missing or the embed call fails. Default `index` (no reroute). |
| `ARBITER_INTENT_LUNA_MODEL` | OpenRouter model id for the live call. Default `openai/gpt-6-luna`. |
| `ARBITER_INTENT_CHEAP_MODEL` | Executor model when difficulty is below the strong cutoff. Empty uses the executing agent's `model`. |
| `ARBITER_INTENT_STRONG_MODEL` | Executor model when difficulty is at or above the strong cutoff. Empty uses `advisor.model`, then the agent's `model`. |
| `ARBITER_INTENT_EMBED_MIN_CONFIDENCE` | Embedding confidence at or above this value routes as `embed` and leaves Luna idle. Below it the embedding choice is still the route for this turn, and Luna runs in the background. Default `0.35`. Clamped to [0, 1]. |
| `ARBITER_INTENT_EMBED_TEMPERATURE` | Softmax temperature on cosine similarity. Default `0.02`. Values outside (0, 5] are ignored. Argmax does not change with temperature; the cutoff does. |
| `ARBITER_EMBED_MODEL` | Operator path to the ONNX file. Must be set together with `ARBITER_EMBED_VOCAB`. Checksum is not applied to an operator path. |
| `ARBITER_EMBED_VOCAB` | Operator path to the WordPiece `vocab.txt`. Pair with `ARBITER_EMBED_MODEL`. |
| `ARBITER_EMBED_NO_FETCH` | When set to anything other than `0` / `false` / `off`, a missing model is not downloaded. Live routing uses `ARBITER_INTENT_DEFAULT_AGENT` for that turn. `ARBITER_OFFLINE` does the same. |

## TUI session durability

| Variable                         | Purpose                                                                 | Default |
|----------------------------------|-------------------------------------------------------------------------|---------|
| `ARBITER_AUTOSAVE_INTERVAL_SEC`  | Periodic dirty flush for conversation files. `0` disables the timer (post-turn and mid-turn `save_async` still run). | `30` |
| `ARBITER_COMPACT_THRESHOLD`      | Integer percent (1–100) of the model context window that triggers auto-compaction. | `75` |
| `ARBITER_COMPACT_DISABLED`       | When set to a non-empty, non-`0` value, disables automatic compaction. `/compact` still works. | unset |

See [`docs/tui/sessions.md`](../tui/sessions.md).

## Host `/exec` (TUI / `--send`)

| Variable / flag              | Purpose |
|------------------------------|---------|
| `--no-exec`                  | Disable host `/exec` in the TUI (default is **enabled** with a confirm gate). |
| `--allow-host-exec`          | Allow host `/exec` when running `arbiter --api` (unsafe; prefer the Docker sandbox). |
| `ARBITER_ALLOW_HOST_EXEC`    | Same as `--allow-host-exec` when set to a non-empty, non-`0` value. |

The API server keeps `/exec` **disabled** unless the per-tenant Docker sandbox is configured (below) or host exec is explicitly allowed. In the TUI, plain argv (`git status`, `ls`, `npm test`, `make`) runs directly, with no shell. Pipes, redirects, substitution, and interpreters running a file or program (`python3 script.py`, `bash script.sh`, `node script.js`, `awk`, `deno run`, including versioned names, absolute paths, and `env`) show a `HOST SHELL (unsandboxed) — destructive command` card; the API's confirm callback denies those instead of prompting. `npm run` and `make` do not prompt. Setting `ARBITER_SANDBOX_IMAGE` in the TUI opts into the same Docker sandbox path as `--api`. If the image is set but the sandbox fails its usability check, `/exec` returns `ERR` (no silent host fallback) until the image is fixed or the env var is unset.

## Per-tenant sandbox

Arbiter's `/exec` writ is disabled by default in the API. Setting `ARBITER_SANDBOX_IMAGE` enables a per-tenant Docker sandbox that confines `/exec` to a workspace volume shared with `/write` and `/read`. The same env set is honoured by the interactive TUI (opt-in Docker `/exec`). The idle reaper (`ARBITER_SANDBOX_IDLE_SECONDS`) is implemented. The full walkthrough is in [`docs/concepts/sandbox.md`](../concepts/sandbox.md); the env-var surface:

| Variable                                | Purpose                                                                                                | Default        |
|-----------------------------------------|--------------------------------------------------------------------------------------------------------|----------------|
| `ARBITER_SANDBOX_IMAGE`                 | Container image to run inside. Required — without this the sandbox stays off and `/exec` returns `ERR`. | unset          |
| `ARBITER_SANDBOX_RUNTIME`               | Runtime binary. v1 supports `docker` only.                                                             | `docker`       |
| `ARBITER_SANDBOX_NETWORK`               | Docker `--network` value. `none` keeps `/exec` offline; `bridge` lets it reach the internet.           | `none`         |
| `ARBITER_SANDBOX_MEMORY_MB`             | Hard memory cap per container, MB. `0` = no cap.                                                       | `512`          |
| `ARBITER_SANDBOX_CPUS`                  | CPU shares per container. `0` = no cap.                                                                | `1.0`          |
| `ARBITER_SANDBOX_PIDS_LIMIT`            | Max processes per container. `0` = no cap.                                                             | `256`          |
| `ARBITER_SANDBOX_EXEC_TIMEOUT`          | Wall-clock kill, seconds, per `/exec` call. `0` = no timeout.                                          | `30`           |
| `ARBITER_SANDBOX_WORKSPACE_MAX_BYTES`   | Per-tenant workspace disk quota, bytes. `/write` over the cap returns ERR; `/exec` snapshots then restores on over-cap or cancel. `0` = no quota. | `1073741824` (1 GiB) |
| `ARBITER_SANDBOX_IDLE_SECONDS`          | Idle threshold before a tenant container is stopped by the background reaper. `0` = no reaping.        | `1800` (30 min) |
| `ARBITER_SANDBOX_WORKSPACES_ROOT`       | Host directory for per-tenant workspace bind mounts.                                                   | `~/.arbiter/workspaces` |

A misconfigured sandbox (docker missing, image string empty, workspaces root unwritable) logs the reason at startup and keeps the server running with `/exec` disabled — the safe default for an exposed API server. Tenant workspaces land at `<workspaces_root>/t<tenant_id>/`.

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
| `tui.log`                  | Operational stderr while the interactive TUI owns the terminal (`openrouter_response`, `intent_route`, `intent_shadow`, and other `fprintf` / `cerr`). Truncated on open once past 1 MiB. Not used by `arbiter --api`. |
| `tui-fatal.log`            | Async-signal-safe crash note from the TUI fatal handlers.            |

Files are read on demand by the relevant subsystem. None of them are watched for changes — restart the process to pick up edits to `agents/*.json` or `mcp_servers.json` while `--api` is running.

## Notes

- `~/.arbiter/` is resolved from `$HOME` (or `getpwuid()` if `$HOME` is unset). `XDG_CONFIG_HOME` is not honoured. To run isolated arbiter instances, override `HOME` for the process: `HOME=/some/other/dir arbiter`.
- No env var controls log level beyond the verbose flag — verbose is binary, on or off.

Anything else arbiter reads is implicit (system clock, locale, timezone) and not configurable.
