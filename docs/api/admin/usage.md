# `GET /v1/admin/usage`

**Auth:** admin — _Status:_ stable

Read raw usage rows from the ledger, newest first. For pre-aggregated rollups (charts, dashboards), use [`GET /v1/admin/usage/summary`](usage-summary.md) instead.

## Request

### Query parameters

| Name        | Type | Default | Description |
|-------------|------|---------|-------------|
| `tenant_id` | int  | 0 (all) | Filter to one tenant. |
| `since`     | int (epoch s) | 0 (no lower bound) | Inclusive. |
| `until`     | int (epoch s) | 0 (no upper bound) | Inclusive. |
| `limit`     | int  | 1000 | Hard max 10000. |

```bash
curl -H "Authorization: Bearer adm_…" \
  "http://arbiter.example.com/v1/admin/usage?tenant_id=1&limit=100"
```

## Response

### 200 OK

```json
{
  "count": 1,
  "entries": [
    {
      "id": 42,
      "tenant_id": 1,
      "timestamp": 1777078022,
      "model": "claude-sonnet-4-6",
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
      "billed_micro_cents": 12870,
      "request_id": "req-c"
    }
  ]
}
```

`provider_micro_cents` is the sum of the four `*_micro_cents` component fields. `billed_micro_cents = provider_micro_cents + markup_micro_cents`. The per-token-type breakdown is captured at write time so historical rows survive pricing-table updates. Field schemas: [Data model → UsageEntry](../concepts/data-model.md#usageentry).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`GET /v1/admin/usage/summary`](usage-summary.md) — bucketed analytics.
- [Billing](../concepts/billing.md).
