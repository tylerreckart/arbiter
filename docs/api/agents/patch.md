# `PATCH /v1/agents/:id`

**Auth:** tenant — _Status:_ stable

**Wholesale replace** of a stored agent's `agent_def` blob. The body has the same shape as [`POST /v1/agents`](create.md); if the body's `id` is set, it must match the path `:id`. The new blob is validated through `Constitution::from_json` before the row is touched, so a bad body never corrupts a stored agent.

There is no field-level merge — the front-end is the source of truth and resends the full blob on every change. For mid-thread one-off overrides without persisting, use inline `agent_def` on the orchestrate call instead.

## Request

| Path param | Type   | Description |
|------------|--------|-------------|
| `id`       | string | Stored agent id. `"index"` is rejected (built-in master is immutable). |

### Body

Same shape and constraints as [`POST /v1/agents`](create.md). All fields required as if creating from scratch.

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"id":"researcher","name":"Researcher v2","role":"researcher","model":"claude-sonnet-4-6","goal":"answer factual questions, cite primary sources"}' \
  http://arbiter.example.com/v1/agents/researcher
```

## Response

### 200 OK

The updated agent, with `created_at` preserved and `updated_at` bumped. Same shape as [`GET /v1/agents/:id`](get.md).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a valid constitution; body `id` doesn't match path; `:id == "index"`. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | No agent with this id for this tenant. | `{"error": "no agent '<id>' for this tenant"}` |

## See also

- [`POST /v1/agents`](create.md) — initial create.
- [`GET /v1/agents/:id`](get.md), [`DELETE /v1/agents/:id`](delete.md).
