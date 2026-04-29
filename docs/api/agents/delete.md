# `DELETE /v1/agents/:id`

**Auth:** tenant — _Status:_ stable

Remove a stored agent. Does **not** cascade to conversations that snapshotted its `agent_def` — those threads keep working off the snapshot. The agent's file scratchpad at `~/.arbiter/memory/t<tenant>/<agent_id>.md` is also kept (tied to the id, not the catalog row).

## Request

| Path param | Type   | Description |
|------------|--------|-------------|
| `id`       | string | Stored agent id. `"index"` is rejected. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/agents/researcher
```

## Response

### 200 OK

```json
{ "deleted": true }
```

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | `:id == "index"`. | `{"error": "cannot delete the built-in master 'index'"}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | No agent with this id for this tenant. | `{"error": "no agent '<id>' for this tenant"}` |

## See also

- [`POST /v1/agents`](create.md), [`PATCH /v1/agents/:id`](patch.md).
- [Tenants → Isolation](../concepts/tenants.md) — tenant-scoping invariants.
