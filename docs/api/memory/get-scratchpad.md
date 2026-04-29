# `GET /v1/memory/:agent_id`

**Auth:** tenant — _Status:_ stable

Read one agent's persistent memory file (or the shared pipeline scratchpad). Use `:agent_id = "shared"` to read `shared.md`.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `agent_id` | string | Agent id, or `"shared"` for the pipeline scratchpad. Validated as `[A-Za-z0-9_-]{1,64}`. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/researcher
```

## Response

### 200 OK

```json
{
  "agent_id": "researcher",
  "kind": "agent",
  "exists": true,
  "size": 1247,
  "content": "# researcher memory\n\n- 2026-04-24T14:00Z: looked up Paris → France\n- 2026-04-24T14:05Z: looked up Berlin → Germany\n…"
}
```

If the agent has never written memory, `exists: false` and `content: ""` — **not** a 404. Rendering a "(no memory yet)" state in your UI doesn't need a special path.

The `content` is the raw markdown file — it's whatever the agent wrote via `/mem write`, including the timestamp headers arbiter's `cmd_mem_write` injects.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | `:agent_id` fails the safety check (`..`, `/`, control chars, empty, > 64 chars). | `{"error": "invalid agent id"}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 503    | Memory not configured on the server. | `{"error": "memory not configured on this server"}` |

## See also

- [`GET /v1/memory`](list-scratchpads.md) — list all scratchpads for this tenant.
- [Structured memory](../concepts/structured-memory.md) — the typed graph.
