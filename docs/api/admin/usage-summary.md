# `GET /v1/admin/usage/summary`

**Auth:** admin — _Status:_ stable

Pre-aggregated rollups for analytics. Saves the sibling service from pulling thousands of raw rows to render a chart.

## Request

### Query parameters

Same as [`GET /v1/admin/usage`](usage.md) — `tenant_id`, `since`, `until` — except `limit` is ignored, plus:

| Name       | Type   | Values                        | Default   | Description |
|------------|--------|-------------------------------|-----------|-------------|
| `group_by` | string | `"model"`, `"day"`, `"tenant"` | `"model"` | Bucket key for the aggregation. |

`day` keys are `"YYYY-MM-DD"` in UTC. `tenant` keys are the tenant id as a string. Buckets are returned sorted by `provider_micro_cents` descending (biggest spenders first).

```bash
curl -H "Authorization: Bearer adm_…" \
  "http://arbiter.example.com/v1/admin/usage/summary?group_by=day&since=1777000000"
```

## Response

### 200 OK

```json
{
  "group_by": "model",
  "count": 2,
  "buckets": [
    {
      "key": "claude-sonnet-4-6",
      "calls": 1,
      "input_tokens": 2200,
      "output_tokens": 150,
      "cache_read_tokens": 0,
      "cache_create_tokens": 500,
      "input_micro_cents": 6600,
      "output_micro_cents": 2250,
      "cache_read_micro_cents": 0,
      "cache_create_micro_cents": 1875,
      "provider_micro_cents": 10725,
      "markup_micro_cents": 2145,
      "billed_micro_cents": 12870
    }
  ]
}
```

## Chart recipes

- **Spend by model (pie):** `group_by=model`, use `billed_micro_cents` per bucket.
- **Spend over time (line):** `group_by=day&since=<30d ago>`, plot `billed_micro_cents` per key.
- **Where spend goes (stacked bar):** `group_by=model`, stack `input_micro_cents` / `output_micro_cents` / `cache_read_micro_cents` / `cache_create_micro_cents` per bucket.
- **Per-tenant rollup:** `group_by=tenant`, list tenants by `billed_micro_cents`.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | `group_by` value not in allowed set. | `{"error": "..."}` |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`GET /v1/admin/usage`](usage.md) — raw rows.
- [Billing](../concepts/billing.md).
