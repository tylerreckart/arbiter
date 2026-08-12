# `POST /v1/intent`

**Auth:** tenant — _Status:_ beta

Stateless classify/route. Same `resolve_intent()` used at depth-0 orchestrator ingress. Does **not** run the agent loop, persist todos, or execute plans.

Use this when an external framework wants Arbiter's hybrid classifier without owning a full `/v1/orchestrate` turn — or to preview what ingress would do.

## Request

### Body

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `message` | string | yes | — | Utterance or event text to classify. |
| `requested_agent` | string | no | `"index"` | Ingress agent. Non-`index` values are treated as explicit (no reroute). |
| `roster` | array | no | tenant + file agents | `{id, role?, goal?, capabilities?}` entries. When omitted, Arbiter uses tenant-stored agents then `agents_dir` (tenant id wins on collision). |
| `intent` | object | no | master's `intent` block | Override `{mode, min_confidence, apply_routing, model}`. |
| `mode` | string | no | master's mode | Shorthand when `intent` is omitted. `off` \| `heuristic` \| `hybrid` \| `llm`. |
| `min_confidence` | number | no | `0.8` | Reroute threshold. |
| `apply_routing` | bool | no | `true` | Classification still runs when false; `applied` will be false. |
| `model` | string | no | `advisor.model` | LLM classifier model for `hybrid` / `llm`. |
| `intent_source` | string | no | — | `"event"` labels `source` as event (used by `/v1/events`). |

```bash
curl http://127.0.0.1:8080/v1/intent \
  -H "Authorization: Bearer $ARBITER_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "message": "Look up primary sources on the RISC-V memory model",
    "requested_agent": "index"
  }'
```

## Response

### 200

```json
{
  "kind": "research",
  "confidence": 0.9,
  "source": "heuristic",
  "target_agent": "research",
  "brief": "Route to research (research).",
  "llm_used": false,
  "malformed": false,
  "applied": true,
  "requested_agent": "index",
  "todo_seeds": [],
  "plan_seeds": []
}
```

| Field | Meaning |
|-------|---------|
| `kind` | Closed taxonomy: `research` \| `review` \| `write` \| `ops` \| `frontend` \| `backend` \| `plan` \| `market` \| `social` \| `multi` \| `unknown`. |
| `source` | `heuristic` \| `llm` \| `explicit` \| `event` \| `none`. |
| `target_agent` | Suggested specialist, or `""` if index should handle it. |
| `applied` | True when `apply_routing` would rewrite an `index` ingress to `target_agent` **and** that id is in the roster used for this request. This endpoint never dispatches. [`POST /v1/orchestrate`](orchestrate.md) additionally requires the agent to be loaded on the request orch (API: tenant catalog; TUI: `agents_dir`). |
| `todo_seeds` / `plan_seeds` | Optional decomposition hints. Not persisted. |

LLM cost, when incurred, is a single history-less complete() on `intent.model` or the master advisor model. Transport/parse failure fail-opens (`target_agent` empty, `malformed` may be true).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400 | Body is not a JSON object, or `message` is missing. | `{"error":"..."}` |
| 401 | Bearer token missing/invalid, or tenant disabled. | `{"error":"..."}` |
| 405 | Method is not POST. | plain text |
| 500 | Orchestrator / client init failed. | `{"error":"..."}` |

## See also

- [Intent concept](../concepts/intent.md)
- [`POST /v1/orchestrate`](orchestrate.md) — full dispatch after the same classifier
- [`POST /v1/advise/gate`](../concepts/advisor.md) — post-turn gate, not ingress
