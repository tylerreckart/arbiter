# `GET /.well-known/agent-card.json`

**Auth:** none — _Status:_ stable

Top-level [Agent2Agent (A2A)](../concepts/a2a.md) discovery stub. Returns a minimal `AgentCard` describing how to authenticate and where to find the per-agent cards. v1.0 of the A2A spec requires `skills.length >= 1`, so the stub carries a single synthetic `discover` skill whose description tells callers to fetch `/v1/a2a/agents/<agent_id>/agent-card.json` with a tenant bearer.

## Request

No path params, no query params, no body, no auth header.

```bash
curl http://arbiter.example.com/.well-known/agent-card.json
```

## Response

### 200 OK

`Content-Type: application/json`. Example body:

```json
{
  "protocolVersion": "1.0",
  "name": "arbiter",
  "description": "Arbiter multi-tenant A2A endpoint. Agents are tenant-scoped; fetch /v1/a2a/agents/<agent_id>/agent-card.json with a tenant bearer token to discover and call individual agents.",
  "url": "https://arbiter.example.com/v1/a2a",
  "version": "stub",
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
      "id": "discover",
      "name": "discover",
      "description": "exchange a tenant bearer token for per-agent cards at /v1/a2a/agents/<agent_id>/agent-card.json",
      "tags": ["discovery", "auth-required"]
    }
  ],
  "securitySchemes": {
    "bearer": { "type": "http", "scheme": "bearer" }
  },
  "security": [ { "bearer": [] } ]
}
```

The `url` field is built from `ApiServerOptions::public_base_url` when set; otherwise it's derived from the inbound `Host` header with scheme `http://`. Operators terminating TLS in front of arbiter should set `public_base_url` explicitly so the card advertises the public `https://` origin.

## Failure modes

| Status | When |
|--------|------|
| 404    | Wrong path. |
| 5xx    | Process crash or upstream proxy misconfig. The handler itself is pure transform — no I/O beyond the Host header read — so 5xx here means the server is broken. |

## See also

- [A2A protocol concept](../concepts/a2a.md)
- [Per-agent card endpoint](agent-card.md)
- [JSON-RPC dispatch](dispatch.md)
