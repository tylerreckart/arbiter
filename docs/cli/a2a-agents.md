# Outbound A2A

Configure remote [Agent2Agent (A2A)](../api/concepts/a2a.md) agents that arbiter's own agents can delegate to via the `/a2a` slash command. Symmetric to `~/.arbiter/mcp_servers.json`: optional file, per-request lifecycle, no long-lived state. Inbound A2A — exposing arbiter agents as A2A endpoints — is documented separately under [`docs/api/concepts/a2a.md`](../api/concepts/a2a.md).

## Registry file

`ApiServerOptions::a2a_agents_path` defaults to `~/.arbiter/a2a_agents.json`. Missing file = no remote agents configured (`/a2a list` returns "no remote agents configured" rather than failing the request). Malformed file throws at registry-load time so the operator sees the error in the API server log.

```json
{
  "agents": {
    "weatherbot": {
      "url": "https://weather.example.com/v1/a2a/agents/weather",
      "auth": {
        "type": "bearer",
        "token_env": "WEATHERBOT_TOKEN"
      }
    },
    "rfc-reviewer": {
      "url": "https://rfc.example.com/a2a/reviewer",
      "auth": {
        "type": "bearer",
        "token": "atr_…"
      }
    }
  }
}
```

| Field            | Type     | Required | Description |
|------------------|----------|----------|-------------|
| `url`            | string   | yes      | The remote's JSON-RPC POST endpoint. The agent card is fetched from `<url>/agent-card.json`. |
| `auth.type`      | string   | no       | Only `"bearer"` is supported in v1; any other value fails registry load. Defaults to `"bearer"` when omitted. |
| `auth.token_env` | string   | no       | Name of an environment variable holding the bearer. **Recommended** for production — tokens never land on disk. |
| `auth.token`     | string   | no       | Bearer inline. Useful for local testing only; checked into the registry file means checked into your config repo. |

If the env var named by `token_env` isn't set, registry load logs a warning and keeps the agent registered with an empty token — calls will fail with HTTP 401 until the var is exported. This avoids bricking the whole server when one credential rotates.

## `/a2a` slash command

Three subcommands, dispatched in `commands.cpp` and routed through the per-request `a2a::Manager`. Available wherever the manager is wired — i.e., any path that builds a request orchestrator (`/v1/orchestrate`, `/v1/conversations/:id/messages`, `/v1/agents/:id/chat`, `/v1/a2a/agents/:id`). The CLI/REPL mode is **not** wired; agents in those contexts get `ERR: A2A unavailable in this context`.

| Command                         | Effect |
|---------------------------------|--------|
| `/a2a list`                     | List configured remote agents with their card descriptions and skill summaries. |
| `/a2a card <name>`              | Render one remote agent's card in detail (skills, description, version, URL). |
| `/a2a call <name> <message>`    | Synchronous `message/send` to the remote. Returns the assistant's reply text. |

Each response lands in a `[/a2a …] … [END A2A]` tool-result block in the agent's next turn, framed identically to `/mcp`, `/fetch`, etc. Bodies are capped at the same 16 KB per-turn limit; longer remote responses are truncated with `... [truncated]`.

A typical session:

```
/a2a list
/a2a call weatherbot what's the weather like in SF tomorrow?
```

The agent emits the slash command verbatim; the dispatcher resolves it and feeds the result back as the next user turn.

## Index routing

When the registry resolves to a non-empty list, the master orchestrator's preamble injects a `REMOTE A2A AGENTS` section right after the local `AGENTS` block:

```
AGENTS — delegate with /agent <id> <task>:
  research [research analyst] sonnet-4-6 — extract decisions...
  reviewer [code reviewer] sonnet-4-6 — pull-request review...

REMOTE A2A AGENTS — delegate with /a2a call <name> <message> (distinct trust boundary; no shared memory):
  weatherbot — weather forecasts and historical climate (skills: chat, fetch-url, web-search)
  rfc-reviewer — IETF RFC review and standards analysis (skills: chat)
```

`index` sees both lists at routing time and chooses between local sub-agents (which share tenant memory) and remote A2A agents (which don't). The "distinct trust boundary; no shared memory" caveat is rendered verbatim so the agent's reasoning is informed by the constraint.

## Trust posture

Calls to a remote A2A agent send only the message text. The remote does **not** see:

- the calling tenant's bearer token (a per-remote token from the registry is sent instead),
- the structured-memory graph,
- local artifacts,
- other agents in the catalog.

The remote's reply lands in the calling agent's tool-result block. Whether to persist it (via `/mem add entry` or `/write`) is the calling agent's decision. The trust boundary is enforced by being on the remote side — there's no shared state to leak.

## Failure modes

| Symptom | Cause | Surface |
|---------|-------|---------|
| `ERR: no remote agent named 'X'` | `/a2a card` or `/a2a call` named an unconfigured remote. | Tool-result block; agent retries or falls back to a local agent. |
| `ERR: card fetch failed: <reason>` | The remote's `/agent-card.json` returned non-200, or the body wasn't a valid `AgentCard`. | Tool-result block; agent drops the call. |
| `ERR: transport: <curl error>` | Network-level failure reaching the remote endpoint. | Tool-result block; rate-limit retries via the same dedup cache as `/fetch`. |
| `ERR: HTTP 401: ...` | Token resolved empty (env var unset) or remote rejected it. | Tool-result block; operator action required. |
| `ERR: A2A unavailable in this context` | CLI / REPL run — no `a2a::Manager` is wired. | Tool-result block; agent skips `/a2a`. |

A remote that crashes mid-call leaves the calling task untouched; the next `/a2a call` re-fetches the card if needed.

## Discovery — caching the card

`a2a::Client::card()` lazy-fetches and caches the card. `refresh_card()` forces a re-fetch. Cards aren't time-expired in v1; if a remote bumps its declared `version`, a follow-up call still uses the cached card until the request ends. Per-request lifecycle means the next request fetches fresh.

For remotes whose card path differs from `<url>/agent-card.json` (the v1.0 default), set `url` to the directory and arbiter appends `agent-card.json`. Pre-v1.0 servers using `agent.json` are not supported — arbiter speaks v1.0 only.

## See also

- [A2A protocol concept](../api/concepts/a2a.md) — the inbound surface and the wire-shape reference.
- [`POST /v1/a2a/agents/:id`](../api/a2a/dispatch.md) — what arbiter exposes to remote A2A clients.
- [MCP servers](../api/concepts/mcp.md) — sister registry pattern for the MCP toolchain.
