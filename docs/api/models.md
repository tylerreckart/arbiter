# `GET /v1/models`

**Auth:** tenant — _Status:_ stable

List the models arbiter knows how to route. Powers the frontend's model picker. The catalogue changes only when the operator deploys a new build — clients should cache it briefly and re-fetch on a fresh session.

Hosted traffic routes through OpenRouter; catalogue ids are OpenRouter slugs (plus a few short Claude aliases rewritten to dotted Anthropic slugs at request time).

## Request

No path params, no query params, no body.

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/models
```

## Response

### 200 OK

```json
{
  "count": 24,
  "models": [
    { "id": "anthropic/claude-sonnet-5",     "provider": "openrouter" },
    { "id": "anthropic/claude-opus-5",       "provider": "openrouter" },
    { "id": "openai/gpt-5.5",                "provider": "openrouter" },
    { "id": "openai/gpt-5.6-sol",            "provider": "openrouter" },
    { "id": "google/gemini-3.6-flash",       "provider": "openrouter" },
    { "id": "x-ai/grok-4.5",                 "provider": "openrouter" },
    { "id": "claude-sonnet-4-6",             "provider": "openrouter" }
  ]
}
```

| Field      | Type   | Description |
|------------|--------|-------------|
| `id`       | string | Matches what you pass in `agent_def.model` (or as the model on a stored agent). |
| `provider` | string | Today: `openrouter` for hosted models (Ollama ids use the `ollama/` prefix and are not listed here). |

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`POST /v1/orchestrate`](orchestrate.md) — `agent_def.model` is validated against this catalogue at request time.
