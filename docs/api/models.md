# `GET /v1/models`

**Auth:** tenant — _Status:_ stable

List the models arbiter knows how to price + route. Powers the frontend's model picker. The response is the full pricing table — clients should cache it briefly (it changes only when the operator deploys a new build) but should re-fetch on a fresh session.

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
  "count": 41,
  "models": [
    {
      "id": "claude-opus-4-7",
      "provider": "anthropic",
      "input_per_mtok_usd": 5.0,
      "output_per_mtok_usd": 25.0,
      "cache_read_per_mtok_usd": 0.5,
      "cache_create_per_mtok_usd": 6.25,
      "supports_caching": true
    }
  ]
}
```

| Field                       | Type    | Description |
|-----------------------------|---------|-------------|
| `id`                        | string  | Matches what you pass in `agent_def.model` (or as the model on a stored agent). |
| `provider`                  | string  | Inferred from the id: `anthropic`, `openai`, `ollama`. |
| `input_per_mtok_usd`        | number  | List-price USD per 1M input tokens (before arbiter's 20% markup). |
| `output_per_mtok_usd`       | number  | List-price USD per 1M output tokens. |
| `cache_read_per_mtok_usd`   | number  | Cached-input rate. Only meaningful when `supports_caching = true`. |
| `cache_create_per_mtok_usd` | number  | Cache-write rate. Only meaningful when `supports_caching = true`. |
| `supports_caching`          | boolean | Whether the family bills cache reads/writes separately. |

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [Billing](concepts/billing.md) — markup math and ledger semantics.
- [`POST /v1/orchestrate`](orchestrate.md) — `agent_def.model` is validated against this catalogue at request time.
