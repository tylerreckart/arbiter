# Authentication

Two token kinds, both presented as `Authorization: Bearer TOKEN`:

| Prefix  | Purpose | Endpoints |
|---------|---------|-----------|
| `atr_…` | **Tenant token** — drives `/v1/orchestrate`, meters against the tenant's cap and ledger. | All non-admin `/v1/*` routes |
| `adm_…` | **Admin token** — read/write tenants and usage data. | `/v1/admin/*` |

Cross-presentation is rejected: an admin token on `/v1/orchestrate` returns `401`, and a tenant token on an admin route returns `401`.

## Admin token provisioning

On first `arbiter --api` start:

1. If `$ARBITER_ADMIN_TOKEN` is set, use it.
2. Otherwise, if `~/.arbiter/admin_token` exists (mode 0600), read it.
3. Otherwise, **generate** a new admin token, write it to `~/.arbiter/admin_token` at mode 0600, and print it once on stdout.

Subsequent starts reuse the file. Override at runtime by setting the env var.

## Tenant token provisioning

Plaintext tenant tokens are returned **only** in the response to:

- [`POST /v1/admin/tenants`](../admin/tenants-create.md)
- `arbiter --add-tenant <name>` (CLI)

The database stores only the SHA-256 digest. If a tenant loses their token, issue a new one (delete-and-recreate or via a future rotate endpoint).

## Failure modes

| Code | When |
|------|------|
| `401` | Header missing, malformed, wrong prefix for the route, token unknown, or tenant `disabled=true`. Same status code in all cases — no oracle for which failure mode applied. |
| `503` | Admin route called while the server has no admin token configured (defensive; shouldn't reach prod). |

## See also

- [Tenants](tenants.md)
- [`POST /v1/admin/tenants`](../admin/tenants-create.md)
- [Operational notes](operations.md)
