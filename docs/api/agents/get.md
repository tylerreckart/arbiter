# `GET /v1/agents/:id`

**Auth:** tenant — _Status:_ stable

Fetch one agent's constitution. `:id = "index"` returns the built-in master; any other id is looked up against the tenant's stored catalog.

## Request

| Path param | Type   | Description |
|------------|--------|-------------|
| `id`       | string | Agent id. `"index"` for the master, or a stored agent's caller-chosen id. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/agents/researcher
```

## Response

### 200 OK

Same shape as a list entry — see [`GET /v1/agents`](list.md). Stored agents include `created_at` and `updated_at`; the built-in `index` does not.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |
| 404    | Id not found in the tenant's catalogue (and not `"index"`). | `{"error": "no agent '<id>' for this tenant"}` |

Inline agents from `agent_def` are not catalogued — they exist for one call (or are snapshotted onto a conversation), neither of which surfaces here.

## See also

- [`GET /v1/agents`](list.md), [`PATCH /v1/agents/:id`](patch.md), [`DELETE /v1/agents/:id`](delete.md).
