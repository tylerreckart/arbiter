# Tenants

A **tenant** is a named identity tied to a single API token. Each carries:

- An opaque API token (shown once at creation, stored only as a SHA-256 digest).
- A `disabled` flag for admin kill-switches.

Billing — eligibility checks, rate cards, caps, credits, invoicing — is delegated to an external billing service when `ARBITER_BILLING_URL` is configured. The runtime tenant has no money fields of its own; the matching billing-service workspace_id is resolved per request via `POST /v1/runtime/auth/validate` and cached.

## Provisioning

Two paths to create a tenant:

- HTTP: [`POST /v1/admin/tenants`](../admin/tenants-create.md) (admin auth required).
- CLI: `arbiter --add-tenant <name>`.

Both return the plaintext token exactly once. The DB stores only the digest — if a token is lost, issue a new one. Provision the matching workspace + token in your billing service separately, then hand the runtime the same `atr_…` bearer.

## Authentication

Tenants present their token via `Authorization: Bearer atr_…` on every request. Admin tokens (`adm_…`) are required for `/v1/admin/*` routes; cross-presentation (admin token on tenant routes or vice versa) returns `401`. See [Authentication](authentication.md) for the full table.

## Isolation

Every endpoint enforces `tenant_id` match. ID leaks across tenants surface as `404`, never as data exposure. This applies to:

- Conversations and messages (see [Conversations](../conversations/create.md)).
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
- [`POST /v1/admin/tenants`](../admin/tenants-create.md)
- [`PATCH /v1/admin/tenants/:id`](../admin/tenants-patch.md)
