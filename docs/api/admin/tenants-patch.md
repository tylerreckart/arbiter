# `PATCH /v1/admin/tenants/:id`

**Auth:** admin — _Status:_ stable

Update a tenant. Both fields optional; apply whichever are present.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Tenant id. |

### Body

| Field                     | Type    | Description |
|---------------------------|---------|-------------|
| `disabled`                | boolean | Set `true` to block new requests from this tenant (`401` on the orchestrate path). |
| `monthly_cap_usd`         | number  | New monthly cap in USD. 0 = unlimited. |
| `monthly_cap_micro_cents` | integer | Same cap in µ¢. Takes precedence over `monthly_cap_usd` if both. |

```bash
curl -X PATCH \
  -H "Authorization: Bearer adm_…" \
  -H "Content-Type: application/json" \
  -d '{"monthly_cap_usd":50}' \
  http://arbiter.example.com/v1/admin/tenants/3
```

## Response

### 200 OK

The updated `Tenant` object.

## Cap change semantics

Cap changes take effect at the next `record_usage` call. In-flight turns that already passed the pre-flight cap check continue to completion; the new cap gates the next turn.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; non-numeric cap. | `{"error": "..."}` |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist. | `{"error": "tenant not found"}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`GET /v1/admin/tenants/:id`](tenants-get.md), [`POST /v1/admin/tenants`](tenants-create.md).
- [Billing → Caps](../concepts/billing.md#caps).
