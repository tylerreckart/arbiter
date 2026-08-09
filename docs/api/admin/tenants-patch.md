# `PATCH /v1/admin/tenants/:id`

**Auth:** admin — _Status:_ stable

Update a tenant. The only mutable field today is `disabled`.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Tenant id. |

### Body

| Field      | Type    | Required | Description |
|------------|---------|----------|-------------|
| `disabled` | boolean | yes      | Set `true` to block new requests from this tenant (`401` on the orchestrate path). Bodies missing this field, or with a non-boolean value, return `400` (no silent no-op). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer adm_…" \
  -H "Content-Type: application/json" \
  -d '{"disabled":true}' \
  http://arbiter.example.com/v1/admin/tenants/3
```

## Response

### 200 OK

The updated `Tenant` object. Flipping `disabled=true` immediately cancels every in-flight orchestration for the tenant (the kill switch is hot, not advisory).

A successful update appends an `update_tenant` row to the [admin audit log](audit.md) with `before={disabled: <prior>}` and `after={disabled: <new>}` so the change is auditable after the fact.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; missing or non-boolean `disabled`. | `{"error": "..."}` |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist. | `{"error": "tenant not found"}` |
| 503    | Admin not configured. | `{"error": "admin not configured"}` |

## See also

- [`GET /v1/admin/tenants/:id`](tenants-get.md), [`POST /v1/admin/tenants`](tenants-create.md).
- [`GET /v1/admin/audit`](audit.md) — read back the audit trail.
- [Tenant admin CLI](../../cli/tenants.md) — `arbiter --disable-tenant` (DB-only; does not cancel in-flight streams on a running `--api` process).
- [Authentication → Kill-switch model](../../concepts/authentication.md#kill-switch-model) — `TenantGate` preflight vs admin hot cancel.
