# Tenants

A **tenant** is a named identity tied to a single API token. Each carries:

- An opaque API token (shown once at creation, stored only as a SHA-256 digest).
- A `disabled` flag for admin kill-switches.

## Provisioning

Two paths to create a tenant:

- HTTP: [`POST /v1/admin/tenants`](../api/admin/tenants-create.md) (admin auth required).
- CLI: `arbiter --add-tenant <name>`.

Both return the plaintext token exactly once. The DB stores only the digest — if a token is lost, issue a new one.

## Authentication

Tenants present their token via `Authorization: Bearer atr_…` on every request. Admin tokens (`adm_…`) are required for `/v1/admin/*` routes; cross-presentation (admin token on tenant routes or vice versa) returns `401`. See [Authentication](authentication.md) for the full table.

## Isolation

Every endpoint enforces `tenant_id` match. ID leaks across tenants surface as `404`, never as data exposure. This applies to:

- Conversations and messages (see [Conversations](../api/conversations/create.md)).
- Memory entries / relations / artifacts (see [Structured memory](structured-memory.md), [Artifacts](artifacts.md)).
- File scratchpads at `~/.arbiter/memory/t<tenant_id>/`.

## Tenant data model

| Field          | Type    | Notes |
|----------------|---------|-------|
| `id`           | integer | Assigned at creation. Stable. |
| `name`         | string  | Display-only; no uniqueness constraint. |
| `disabled`     | boolean | `true` → all `/v1/orchestrate` calls return 401. |
| `created_at`   | integer | Epoch seconds. |
| `last_used_at` | integer | Epoch seconds. 0 if the tenant has never made a call. |

## See also

- [Authentication](authentication.md)
- [`POST /v1/admin/tenants`](../api/admin/tenants-create.md)
- [`PATCH /v1/admin/tenants/:id`](../api/admin/tenants-patch.md)
