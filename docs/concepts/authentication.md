# Authentication

Two token kinds, both presented as `Authorization: Bearer TOKEN`:

| Prefix  | Purpose | Endpoints |
|---------|---------|-----------|
| `atr_…` | **Tenant token** — drives `/v1/orchestrate` and all non-admin routes. | All non-admin `/v1/*` routes |
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

- [`POST /v1/admin/tenants`](../api/admin/tenants-create.md)
- `arbiter --add-tenant <name>` (CLI)

The database stores only the SHA-256 digest. If a tenant loses their token, rotate it with `arbiter --rotate-tenant-token <id|name>` or `POST /v1/admin/tenants/:id/rotate-token`.

## Kill-switch model

Revoking a tenant mid-request uses one durable mechanism — **`TenantGate`** — not a scatter of ad-hoc checks:

1. After bearer auth, expensive handlers build a `TenantGate` from the tenant id + `api_key_hash` snapshot and **`bind()` it to the per-request `ApiClient` preflight**.
2. Every `stream()` / `complete()` (including retries, mid-body reads, and `/parallel` child clients that inherit the preflight) re-probes the DB via `TenantGate::alive()`. Disable or rotate makes `alive()` false → provider I/O stops with `cancelled`.
3. **Admin HTTP** disable / `rotate-token` also cancels `InFlightRegistry` entries immediately (hot path).
4. **CLI** `--disable-tenant` / `--rotate-tenant-token` update SQLite only; running `--api` workers stop at the next preflight (tool-loop / provider-read boundary). Prefer admin HTTP rotate for an immediate cancel().
5. Caller `POST /v1/requests/:id/cancel` stays the `cancelled` taxonomy; tenant revoke surfaces as `unauthorized` on terminal frames when the digest/disabled check fails.

## Failure modes

| Code | When |
|------|------|
| `401` | Header missing, malformed, wrong prefix for the route, token unknown, or tenant `disabled=true`. Same status code in all cases — no oracle for which failure mode applied. |
| `503` | Admin route called while the server has no admin token configured (defensive; shouldn't reach prod). |

## See also

- [Tenants](tenants.md)
- [`POST /v1/admin/tenants`](../api/admin/tenants-create.md)
- [Operational notes](operations.md)
