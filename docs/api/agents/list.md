# `GET /v1/agents`

**Auth:** tenant — _Status:_ stable

List the agents visible to this tenant — the built-in `index` master plus every agent stored for this tenant via [`POST /v1/agents`](create.md). Newest `updated_at` first; `index` is always the head of the list.

Inline agents (passed as `agent_def` on a single [`POST /v1/orchestrate`](../orchestrate.md) call) are **not** listed here — they exist only for that one call and aren't persisted. Persist with `POST /v1/agents` if you want catalog visibility, snapshotting onto conversations, and `/agent`/`/parallel` references by id.

## Request

No path / query / body params.

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/agents
```

## Response

### 200 OK

```json
{
  "count": 2,
  "agents": [
    {
      "id": "index",
      "name": "index",
      "role": "orchestrator",
      "model": "claude-haiku-4-5",
      "goal": "…",
      "brevity": "full",
      "max_tokens": 4096,
      "temperature": 0.3,
      "rules": ["…"],
      "capabilities": ["…"]
    },
    {
      "id": "researcher",
      "name": "Researcher",
      "role": "researcher",
      "model": "claude-sonnet-4-6",
      "goal": "answer one factual question in one short paragraph",
      "brevity": "bullets",
      "max_tokens": 256,
      "temperature": 0.2,
      "rules": [],
      "capabilities": ["research"],
      "created_at": 1777060001,
      "updated_at": 1777060001
    }
  ]
}
```

Stored agents carry `created_at` and `updated_at`; the built-in `index` does not (its constitution is server-controlled and immutable from the API).

Field schemas: see [Data model → Agent](../concepts/data-model.md#agent-catalog-row).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`POST /v1/agents`](create.md), [`GET /v1/agents/:id`](get.md), [`PATCH /v1/agents/:id`](patch.md), [`DELETE /v1/agents/:id`](delete.md), [`POST /v1/agents/:id/chat`](chat.md).
