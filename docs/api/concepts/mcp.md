# MCP servers

Arbiter ships a pluggable [Model Context Protocol](https://modelcontextprotocol.io) client. Operators register external MCP servers — Microsoft's playwright server for browsing, Stripe's MCP for billing, anything that speaks the protocol — and agents reference them mid-turn through `/mcp` slash commands.

The integration is **per-request and stateful within a request**:

- Each [`POST /v1/orchestrate`](../orchestrate.md), [`POST /v1/conversations/:id/messages`](../conversations/messages-post.md), and [`POST /v1/agents/:id/chat`](../agents/chat.md) call gets its own `mcp::Manager` and its own subprocess.
- Within that request, multiple `/mcp` commands share the same browser context — `/mcp call playwright browser_navigate {...}` followed by `/mcp call playwright browser_snapshot` see the same tab.
- When the request ends (or the client cancels via [`/v1/requests/:id/cancel`](../requests-cancel.md)), the manager is destroyed and the subprocess receives `SIGTERM`. State does not persist across requests.

The cold-start cost of spawning a server lazily on first reference is real (`npx @playwright/mcp` is multi-second) but acceptable in exchange for clean tenant isolation and zero zombie-cleanup logic. The first `/mcp` reference per request pays the cost; subsequent ones in the same turn share the live process.

## Registry

Configure available servers via `~/.arbiter/mcp_servers.json` (path comes from `ApiServerOptions::mcp_servers_path` — defaults to `~/.arbiter/mcp_servers.json` when running `arbiter --api`):

```json
{
  "servers": {
    "playwright": {
      "command": "npx",
      "args": ["-y", "@playwright/mcp@latest", "--headless"],
      "env": {
        "PLAYWRIGHT_BROWSERS_PATH": "/var/lib/arbiter/playwright"
      },
      "init_timeout_ms": 90000,
      "call_timeout_ms": 30000
    }
  }
}
```

| Field             | Type           | Required | Description |
|-------------------|----------------|----------|-------------|
| `command`         | string         | yes      | Executable to run. PATH-resolved via `execvp`. |
| `args`            | array<string>  | no       | Arguments after `command`. |
| `env`             | object         | no       | Extra `KEY: "VALUE"` strings appended to the parent environment. |
| `init_timeout_ms` | int            | no       | Wall-clock budget for the JSON-RPC `initialize` handshake. Defaults to 60s — first-run `npx` may install. |
| `call_timeout_ms` | int            | no       | Per-`tools/call` timeout. Defaults to 30s. Playwright snapshots/navigation routinely take 5–15s. |

Missing file = no MCP servers configured (clean ERR from `/mcp` rather than a startup failure). Malformed file throws at registry-load time so the operator sees the error in the API server log immediately. The arbiter binary itself does not install any server — operators bring their own (`npm install -g @playwright/mcp`, etc.).

### Hosted MCP servers (Sentry, Linear, Slack, GitHub Copilot, …)

Arbiter speaks **stdio MCP only**. Hosted services that publish HTTP/SSE MCP endpoints are reachable via [`mcp-remote`](https://www.npmjs.com/package/mcp-remote), an stdio child that proxies JSON-RPC to the HTTP endpoint and handles browser-based OAuth on first connect. The same pattern Claude Desktop, Cursor, and Continue use.

```json
{
  "servers": {
    "sentry": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "https://mcp.sentry.dev/mcp"],
      "init_timeout_ms": 90000
    }
  }
}
```

OAuth lifecycle: the first call to a hosted server triggers `mcp-remote` to print a login URL to stderr and wait for the browser callback. Tokens land at `~/.mcp-auth/` and persist across runs. For a daemonised `arbiter --api`, run `npx mcp-remote <url>` once interactively from a shell to seed the cache before launching the daemon.

A ready-to-use registry covering GitHub, Sentry, Linear, and Slack ships at [`examples/mcp_servers.json`](../../../examples/mcp_servers.json) — copy to `~/.arbiter/mcp_servers.json` and edit. The shipped `backend`, `devops`, `frontend`, `reviewer`, `planner`, and `research` agents declare `/mcp` in their capabilities and carry per-agent rules describing which servers to call for which work.

## Slash commands

Agents drive the catalog via three subcommands. Both `/mcp tools` and `/mcp call` are gated to the request's `mcp::Manager` — agents in CLI/REPL contexts get `ERR: MCP unavailable` and adapt.

| Command | Effect |
|---------|--------|
| `/mcp tools` | List every configured server's tools (lazy-spawns each on first listing). |
| `/mcp tools <server>` | List one server's tools. |
| `/mcp call <server> <tool> [json_args]` | Invoke a tool. `json_args` must be a JSON object if present. |

A typical playwright session inside one agent turn:

```
/mcp call playwright browser_navigate {"url":"https://news.ycombinator.com"}
/mcp call playwright browser_snapshot
/mcp call playwright browser_click {"ref":"link-42"}
/mcp call playwright browser_snapshot
```

Each call's response lands in a `[/mcp call …] … [END MCP]` tool-result block in the agent's next turn, framed identically to `/fetch` and `/exec`. Tool-call results are capped at 16 KB per call (matching `/fetch`); larger snapshots are truncated with `... [truncated]`. Agents that need full content should follow up with narrower queries.

## `/browse <url>` — convenience over playwright

JS-rendering fetch via the configured **playwright** MCP server. Composes two MCP calls under the hood:

1. `playwright/browser_navigate {"url": "..."}` — opens the URL.
2. `playwright/browser_snapshot` — captures the rendered accessibility tree.

The snapshot text is what arrives in the agent's tool-result block; the navigate confirmation is suppressed. On nav failure (timeout, transport ERR, or `isError=true`), the snapshot is skipped and the navigate error surfaces verbatim.

**Requirements:** an MCP server registered as `playwright` in `mcp_servers_path`. Without it, `/browse` returns:

```
ERR: /browse requires a playwright MCP server configured for this deployment.
Adapt: try /fetch <url> instead (works for static pages).
```

**Budget:** `/browse` and `/fetch` share a combined **3 URL reads per turn** — they're alternatives. Cold-start cost on the first `/browse` per request is multi-second (npx-spawning Chromium); subsequent `/browse` calls in the same turn share the live browser context.

### When to escalate to `/browse`

| Symptom from `/fetch` | Escalate? |
|------------------------|-----------|
| Empty body or just nav chrome on a JS-heavy SPA | yes |
| "Just a moment..." (Cloudflare interstitial) | yes |
| Login-wall redirect, no article body | yes |
| Static HTML with the content present | no — keep `/fetch` |

**Don't** `/browse` a URL that `/fetch` already retrieved successfully — that's a wasted browser spawn.

## Output shape

`/mcp tools <server>` renders one tool per line with the first paragraph of its description (truncated at 120 chars). The server's full input schema is *not* inlined — agents that need it should read the corresponding MCP server's docs or call `tools/list` directly via `/mcp call`.

`/mcp call <server> <tool> {...}` concatenates `text` content items from the MCP `tools/call` response. Non-text content (images, embedded resources) is annotated as `[non-text content: <type> (<mimeType>) — agent surfaces only text]` so the agent knows something exists but isn't decoded inline.

## Billing

MCP calls are **not** billed — they don't consume tokens. The agent's tokens spent emitting and consuming the slash command are billed as normal LLM usage; the subprocess itself is server compute. Operators concerned about runaway cost should rate-limit at the proxy or cap concurrent requests per tenant.

## Failure modes

| Symptom | Cause | Surface |
|---------|-------|---------|
| `ERR: no MCP server '<name>' configured` | Slash command names a server not in the registry. | Tool-result block; agent retries / abandons. |
| `ERR: MCP server stopped responding during ...` | Subprocess crashed mid-call or `call_timeout_ms` elapsed. | Tool-result block; subsequent calls re-spawn. |
| `ERR: invalid JSON args: ...` | `json_args` failed to parse or wasn't an object. | Tool-result block; agent re-emits with valid JSON. |
| `ERR: MCP unavailable in this context` | CLI/REPL context — no `mcp::Manager` is wired (HTTP-only feature). | Tool-result block; agent drops the /mcp step. |

A subprocess that crashes mid-request is **not auto-restarted** within that request — the manager keeps the dead `Client` and subsequent calls return ERR. The next request gets a fresh manager and a fresh subprocess. This avoids resurrection loops on a server that's broken for protocol reasons.

## See also

- [Web search](search.md) — `/search` is the discovery counterpart to `/browse`.
- [`POST /v1/orchestrate`](../orchestrate.md)
- [Operational notes](operations.md)
