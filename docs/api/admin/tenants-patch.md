# `PATCH /v1/admin/tenants/:id`

**Auth:** admin — _Status:_ stable

Update a tenant. The only mutable field today is `disabled` — billing-related fields (caps, plans, credits) live in the external billing service.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Tenant id. |

### Body

| Field      | Type    | Description |
|------------|---------|-------------|
| `disabled` | boolean | Set `true` to block new requests from this tenant (`401` on the orchestrate path). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer adm_…" \
  -H "Content-Type: application/json" \
  -d '{"disabled":true}' \
  http://arbiter.example.com/v1/admin/tenants/3
```

## Response

### 200 OK

The updated `Tenant` object.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON. | `{"error": "..."}` |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist. | `{"error": "tenant not found"}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`GET /v1/admin/tenants/:id`](tenants-get.md), [`POST /v1/admin/tenants`](tenants-create.md).
