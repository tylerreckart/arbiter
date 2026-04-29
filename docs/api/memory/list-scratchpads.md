# `GET /v1/memory`

**Auth:** tenant — _Status:_ stable

List the file-scratchpad memory entries for the authenticated tenant. These are the per-agent markdown surfaces written via the agent's `/mem write` slash command, plus the optional shared pipeline scratchpad.

For the structured graph (typed nodes + directed edges), see [`GET /v1/memory/entries`](entries/list.md) instead.

## Request

No path / query / body params.

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory
```

## Response

### 200 OK

```json
{
  "tenant_id": 1,
  "count": 2,
  "entries": [
    {
      "kind": "agent",
      "agent_id": "550e8400-e29b-41d4-a716-446655440000",
      "size": 1247,
      "modified_at": 1777058449
    },
    {
      "kind": "shared",
      "agent_id": "",
      "size": 320,
      "modified_at": 1777058020
    }
  ]
}
```

| Field         | Type    | Description |
|---------------|---------|-------------|
| `kind`        | string  | `"shared"` for the pipeline scratchpad (`shared.md`), `"agent"` for per-agent memory. |
| `agent_id`    | string  | Empty for `shared`; otherwise the agent's id. |
| `size`        | integer | Bytes. |
| `modified_at` | integer | Epoch seconds. |

The endpoint returns an empty list — **not a 404** — when the tenant has never triggered a memory write.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |
| 503    | Memory not configured on the server. | `{"error": "memory not configured on this server"}` |

## See also

- [`GET /v1/memory/:agent_id`](get-scratchpad.md) — read one scratchpad.
- [Structured memory](../concepts/structured-memory.md) — the typed graph that lives alongside this surface.
