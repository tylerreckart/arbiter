# `POST /v1/admin/tenants/:id/rotate-token`

**Auth:** admin — _Status:_ stable

Invalidate the tenant's current API key digest and issue a new plaintext `atr_…` token. The new token is returned **exactly once** in this response — same contract as [`POST /v1/admin/tenants`](tenants-create.md).

Use this when upgrading from single-tenant (no-bearer) mode (tenant rows may exist without a recoverable plaintext key), or whenever a key is lost or leaked.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Tenant id. |

No body.

```bash
curl -X POST \
  -H "Authorization: Bearer adm_…" \
  http://arbiter.example.com/v1/admin/tenants/3/rotate-token
```

## Response

### 200 OK

Tenant object plus the new plaintext `token` field.

```json
{
  "id": 3,
  "name": "acme",
  "disabled": false,
  "created_at": 1777056438,
  "last_used_at": 1777078022,
  "token": "atr_…"
}
```

A successful rotation appends a `rotate_tenant_token` row to the [admin audit log](audit.md). The audit payload does **not** include the plaintext token. In-flight orchestrations for the tenant are cancelled immediately (same hot path as `disabled=true`).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Bad tenant id. | `{"error": "..."}` |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist. | `{"error": "tenant not found"}` |
| 500    | Rotation failed. | `{"error": "token rotation failed"}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`POST /v1/admin/tenants`](tenants-create.md) — create + first token.
- [Tenant admin CLI](../../cli/tenants.md) — `arbiter --rotate-tenant-token`.
