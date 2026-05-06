# `GET /v1/a2a/agents/:id/agent-card.json`

**Auth:** tenant — _Status:_ stable

Returns the [Agent2Agent (A2A)](../concepts/a2a.md) `AgentCard` for one agent. The card is derived at fetch time from the agent's `Constitution`; there's no on-disk cache, so updates to a stored agent (`PATCH /v1/agents/:id`) take effect on the next card fetch.

`:id` resolves against the calling tenant's stored catalog (`tenant_agents`) plus the built-in `index` master:

- `index` — always available; returns the master orchestrator's card.
- Any other id — must exist in the tenant's catalog (provisioned via [`POST /v1/agents`](../agents/create.md)). A stored agent's id is opaque to the URL; cards for other tenants are never reachable.

## Request

Path: `/v1/a2a/agents/:id/agent-card.json`. No body.

```bash
curl -H "Authorization: Bearer atr_…" \
  https://arbiter.example.com/v1/a2a/agents/index/agent-card.json
```

## Response

### 200 OK

`Content-Type: application/json`. Example for the master:

```json
{
  "protocolVersion": "1.0",
  "name": "index",
  "description": "orchestrator — Route tasks to the right agents. Compose multi-agent pipelines when needed. Synthesize results. Produce real output — files, code, reports — not descriptions of output.",
  "url": "https://arbiter.example.com/v1/a2a/agents/index",
  "version": "index",
  "preferredTransport": "JSONRPC",
  "defaultInputModes": ["text/plain"],
  "defaultOutputModes": ["text/plain", "application/json"],
  "capabilities": {
    "streaming": true,
    "pushNotifications": false,
    "stateTransitionHistory": false
  },
  "skills": [
    {
      "id": "chat",
      "name": "chat",
      "description": "free-form text conversation with the agent",
      "tags": ["text"]
    }
  ],
  "securitySchemes": {
    "bearer": { "type": "http", "scheme": "bearer" }
  },
  "security": [ { "bearer": [] } ]
}
```

For a stored agent with declared capabilities, additional `Skill` entries are appended after `chat`:

```json
{
  "skills": [
    { "id": "chat",       "name": "chat",       "description": "free-form text conversation with the agent",                       "tags": ["text"] },
    { "id": "fetch-url",  "name": "fetch-url",  "description": "fetch a URL and return its rendered text content",                "tags": ["/fetch"] },
    { "id": "web-search", "name": "web-search", "description": "run a web search query and summarize the top results",            "tags": ["/search"] },
    { "id": "memory",     "name": "memory",     "description": "read, write, and search the agent's persistent memory",            "tags": ["/mem"] }
  ]
}
```

`tags[0]` is the raw arbiter slash-command form so callers can filter precisely. The mapping from `Constitution.capabilities` to friendly skill ids and descriptions lives in `src/a2a/server.cpp::skill_catalog`.

`version` for the master is the literal `"index"`; for a stored agent it's the catalog row's `updated_at` epoch, so a card consumer can cheap-cache and re-fetch on a version mismatch.

`url` is built from `ApiServerOptions::public_base_url` (or the `Host` header fallback). See the [well-known endpoint](well-known.md) for the same behaviour.

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing or invalid bearer token. |
| 404    | `:id` doesn't match `index` or a stored agent for the calling tenant. |
| 500    | Stored agent's `agent_def_json` blob fails to parse as a `Constitution`. Surfaces as `{"error": "agent_def parse failed: …"}`. The catalog row is preserved so an operator can fix it via `PATCH /v1/agents/:id`. |

## See also

- [A2A protocol concept](../concepts/a2a.md)
- [Well-known discovery stub](well-known.md)
- [JSON-RPC dispatch](dispatch.md)
- [`GET /v1/agents/:id`](../agents/get.md) — the arbiter-native counterpart that returns the raw `Constitution` JSON.
