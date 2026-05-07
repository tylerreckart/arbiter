# Environment

Every environment variable arbiter reads, what it controls, and what overrides what.

## Provider keys

Arbiter routes model calls through whichever provider key is set. At least one is required; multiple can coexist (different agents in `~/.arbiter/agents/*.json` can target different providers).

| Variable                | Used by                              | Fallback                          |
|-------------------------|--------------------------------------|-----------------------------------|
| `ANTHROPIC_API_KEY`     | Any agent whose `model` is a Claude id | `~/.arbiter/api_key` file       |
| `OPENAI_API_KEY`        | Any agent whose `model` is an OpenAI id | `~/.arbiter/openai_api_key` file |
| `OLLAMA_HOST`           | Any agent whose `model` resolves to Ollama | `http://localhost:11434`     |

Env-var values take precedence over the file values. The file is read once at process start; changes during a long-running `--api` session require a restart.

## Server (`--api`) configuration

| Variable                  | Purpose                                                                       |
|---------------------------|-------------------------------------------------------------------------------|
| `ARBITER_API_VERBOSE`     | When set to a non-empty, non-`0` value, mirrors every SSE event to stderr. Equivalent to passing `--verbose`. The CLI flag wins if both are present. |
| `ARBITER_BILLING_URL`     | Base URL of the operator's external billing service. When set, the runtime exchanges every bearer for a workspace via `/v1/runtime/auth/validate`, pre-flights against `/v1/runtime/quota/check`, and posts post-turn telemetry to `/v1/runtime/usage/record`. Unset → no eligibility checks; provider keys go straight through. |

## Web search

Arbiter agents can emit `/search <query>`. To make that route somewhere, configure a provider:

| Variable                       | Purpose                                                      |
|--------------------------------|--------------------------------------------------------------|
| `ARBITER_SEARCH_PROVIDER`      | Provider id. Currently `brave` (Brave Search API) is the implemented provider. Default: unset (search disabled). |
| `ARBITER_SEARCH_API_KEY`       | API key for the configured provider. Preferred — explicitly scoped to arbiter's search use. |
| `BRAVE_SEARCH_API_KEY`         | Convenience fallback when `ARBITER_SEARCH_API_KEY` is unset. Useful if you already have this var set for other tools. |

Without a key configured, `/search` returns `ERR` and the agent falls back to `/fetch` on URLs it already knows.

## Precedence summary

For each setting, the order arbiter checks (first hit wins):

1. **CLI flag** — `--port`, `--bind`, `--verbose`.
2. **`ARBITER_*` env var** — preferred for arbiter-specific config.
3. **Convention env var** — `BRAVE_SEARCH_API_KEY`, `ANTHROPIC_API_KEY`, etc.
4. **`~/.arbiter/<file>`** — convenient for keys, less convenient for runtime config.
5. **Hard-coded default** — `127.0.0.1`, `8080`, `localhost:11434`.

## Files under `~/.arbiter/`

Distinct from env vars but listed here for completeness, since the env-vs-file precedence question is the most common operational confusion:

| Path                       | Purpose                                                              |
|----------------------------|----------------------------------------------------------------------|
| `api_key`                  | Anthropic API key (one line, no whitespace).                         |
| `openai_api_key`           | OpenAI API key.                                                      |
| `admin_token`              | Admin token used by `/v1/admin/*`. Generated automatically on first `--api` launch if missing. |
| `tenants.db`               | Tenant identity store (SQLite).                                      |
| `agents/*.json`            | Agent constitutions.                                                 |
| `sessions/*.json`          | Per-cwd interactive session snapshots.                               |
| `memory/<agent>/notes.md`  | Per-agent persistent scratchpad (`/mem write`).                      |
| `mcp_servers.json`         | Optional MCP server registry. See [`docs/concepts/mcp.md`](../concepts/mcp.md). |
| `history`                  | Merged TUI editor history across panes.                              |

Files are read on demand by the relevant subsystem. None of them are watched for changes — restart the process to pick up edits to `agents/*.json` or `mcp_servers.json` while `--api` is running.

## Notes

- `~/.arbiter/` is resolved from `$HOME` (or `getpwuid()` if `$HOME` is unset). `XDG_CONFIG_HOME` is not honoured. To run isolated arbiter instances, override `HOME` for the process: `HOME=/some/other/dir arbiter`.
- No env var controls log level beyond the verbose flag — verbose is binary, on or off.

Anything else arbiter reads is implicit (system clock, locale, timezone) and not configurable.
