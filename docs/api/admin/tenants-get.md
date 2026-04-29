# `GET /v1/admin/tenants/:id`

**Auth:** admin — _Status:_ stable

Fetch one tenant. Same shape as a list entry.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Tenant id. |

```bash
curl -H "Authorization: Bearer adm_…" \
  http://arbiter.example.com/v1/admin/tenants/3
```

## Response

### 200 OK

The `Tenant` object — see [Data model → Tenant](../concepts/data-model.md#tenant). The plaintext token is **not** returned (it's only available at creation time).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist. | `{"error": "tenant not found"}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`PATCH /v1/admin/tenants/:id`](tenants-patch.md).
