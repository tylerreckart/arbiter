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
| `advisor`       | object \| string | no | Advisor configuration. Object form: `{model, prompt?, mode?, max_redirects?, malformed_halts?}`. String form is treated as `{model: <s>, mode: "consult"}` (back-compat). See [advisor concept](../concepts/advisor.md). |
| `advisor_model` | string | no  | **Legacy.** Higher-capability model for `/advise` consults. New configurations should use `advisor.model` instead. If both `advisor` and `advisor_model` are present, the structured `advisor` block wins. |
| `memory`        | object | no  | Per-agent memory enrichment toggles for `/mem search` and `/mem add entry`. See schema below and [Memory enrichment](../concepts/structured-memory.md#memory-enrichment) in the structured-memory concept. |
| `personality`   | string | no  | Free-form personality overlay. |

#### `advisor` object schema

| Sub-field          | Type    | Default     | Notes |
|--------------------|---------|-------------|-------|
| `model`            | string  | (required when `mode != "off"`) | Higher-capability model used for consults and gate decisions. |
| `mode`             | string  | `"consult"` | `"off"` disables both consult and gate; `"consult"` makes `/advise` available to the executor; `"gate"` additionally gates the executor's terminating turn — the runtime calls the advisor with the executor's output and a tool summary, and only returns to the caller on a `CONTINUE` signal. See [advisor concept](../concepts/advisor.md). |
| `prompt`           | string  | built-in    | Override the gate's system prompt. Only consulted in `mode: "gate"`. |
| `max_redirects`    | int     | `2`         | Cap on how many `REDIRECT` signals the gate can issue per top-level turn before the runtime synthesises a `HALT`. |
| `malformed_halts`  | bool    | `true`      | Whether an unparseable advisor reply should be treated as `HALT` (`true` — fail-closed) or `CONTINUE` (`false` — fail-open). |

#### `memory` object schema

Controls advisor-driven enrichment on this agent's `/mem` operations. Every field is optional; absent fields fall through to the documented default. The advisor-driven toggles (`search_expand`, `auto_tag`, `auto_supersede`) need an `advisor.model` configured — without one they silently no-op.

| Sub-field          | Type | Default | Notes |
|--------------------|------|---------|-------|
| `search_expand`    | bool | `false` | On `/mem search`: call the advisor once to generate 2 paraphrases of the query, run all 3 variants through FTS, RRF-fuse the rankings. No-embedding alternative to dense retrieval. ~150 ms + ~$0.0001 per search at Haiku speeds. |
| `auto_tag`         | bool | `false` | On `/mem add entry`: advisor extracts 2-4 lowercase hyphenated tags from `title` + `content` before storage. Tags get an 8× weight in the BM25 ranking, so this is one of the strongest no-cost ways to lift retrieval signal on agent ingest paths. |
| `auto_supersede`   | bool | `false` | On `/mem add entry`: after the new entry is created, advisor inspects the top-5 same-type FTS hits on the new title for direct contradictions and stamps `valid_to=now()` on flagged ids. Bias is conservative — false positives erase legitimate prior memory. |
| `intent_routing`   | bool | `true`  | On `/mem search`: heuristic regex-based question-intent classifier maps cue words ("favorite", "when", "how to", …) to memory-entry type boosts via the existing 1.3× BM25 multiplier. Caller-supplied `type=` always wins. Zero LLM cost; defaults on because the worst case is "no boost applied" (monotonic vs. off). |

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
