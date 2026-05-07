# `GET /v1/models`

**Auth:** tenant — _Status:_ stable

List the models arbiter knows how to route. Powers the frontend's model picker. The catalogue changes only when the operator deploys a new build — clients should cache it briefly and re-fetch on a fresh session.

Pricing is not included in the runtime response — it lives in the configured billing service's rate card. Frontends that need cost-per-token figures should fetch them from the billing service directly.

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
  "count": 15,
  "models": [
    { "id": "claude-opus-4-7",          "provider": "anthropic" },
    { "id": "claude-sonnet-4-6",        "provider": "anthropic" },
    { "id": "openai/gpt-5.4",           "provider": "openai" },
    { "id": "gemini/gemini-2.5-pro",    "provider": "gemini" },
    { "id": "gemini/gemini-2.5-flash",  "provider": "gemini" }
  ]
}
```

| Field      | Type   | Description |
|------------|--------|-------------|
| `id`       | string | Matches what you pass in `agent_def.model` (or as the model on a stored agent). |
| `provider` | string | `anthropic`, `openai`, `gemini`, or `ollama`. |

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`POST /v1/orchestrate`](orchestrate.md) — `agent_def.model` is validated against this catalogue at request time.
