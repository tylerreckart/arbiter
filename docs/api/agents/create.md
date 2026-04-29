# `POST /v1/agents`

**Auth:** tenant — _Status:_ stable

Create a stored agent for this tenant. The `id` is caller-chosen — typically a UUID owned by the sibling service. Two tenants may independently use the same `id` without collision.

Stored agents are visible from the [agents catalog](list.md), addressable from `/agent` and `/parallel` slash commands inside other agents' turns, and snapshotted onto conversations created against them.

## Request

### Body

Either a bare constitution or wrapped under `agent_def`:

```json
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
  "capabilities": ["research"]
}
```

| Field | Type | Required | Constraint |
|-------|------|----------|------------|
| `id`            | string | yes | `[A-Za-z0-9_-]{1,64}`, unique per tenant, `id != "index"`. Becomes a path component for the file scratchpad and a token in slash-command syntax. |
| `name`          | string | yes | Free-form display name. |
| `role`          | string | yes | Short role descriptor (e.g. `"code reviewer"`). |
| `model`         | string | yes | Routed by arbiter's provider prefix table — see [`GET /v1/models`](../models.md). |
| `goal`          | string | yes | What this agent is trying to accomplish. Goes into its system prompt. |
| `brevity`       | string | no  | `"lite"` \| `"full"` \| `"ultra"`. Default `"full"`. |
| `max_tokens`    | int    | no  | Response cap per turn. Default 1024. |
| `temperature`   | number | no  | 0.0–2.0. Default 0.3. |
| `rules`         | array<string> | no | Behavioural constraints appended to the system prompt. |
| `capabilities`  | array<string> | no | Tools this agent uses. Used by master for routing. |
| `mode`          | string | no  | `""` / `"standard"` (default) or `"writer"`. |
| `advisor_model` | string | no  | Higher-capability model for `/advise` consults. |
| `personality`   | string | no  | Free-form personality overlay. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"id":"researcher","name":"Researcher","role":"researcher","model":"claude-sonnet-4-6","goal":"answer factual questions"}' \
  http://arbiter.example.com/v1/agents
```

## Response

### 201 Created

The stored agent with `created_at` / `updated_at`. The full blob is persisted; the response is rendered by re-parsing it through `Constitution::from_json`, so what comes back is exactly what's stored.

```json
{
  "id": "researcher",
  "name": "Researcher",
  "role": "researcher",
  "model": "claude-sonnet-4-6",
  "goal": "answer factual questions",
  "brevity": "full",
  "max_tokens": 1024,
  "temperature": 0.3,
  "rules": [],
  "capabilities": [],
  "created_at": 1777060001,
  "updated_at": 1777060001
}
```

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a valid constitution (`Constitution::from_json` failed); `id` missing / malformed; `id == "index"`. | `{"error": "..."}` |
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |
| 409    | Duplicate `id` for this tenant. | `{"error": "agent '<id>' already exists for this tenant", "existing": { ...AgentRecord... }}` |

To replace an existing stored agent, use [`PATCH /v1/agents/:id`](patch.md).

## See also

- [`GET /v1/agents`](list.md), [`GET /v1/agents/:id`](get.md), [`PATCH /v1/agents/:id`](patch.md), [`DELETE /v1/agents/:id`](delete.md), [`POST /v1/agents/:id/chat`](chat.md).
- [`POST /v1/orchestrate`](../orchestrate.md) — for inline (non-persisted) agents.
