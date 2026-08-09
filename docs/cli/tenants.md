# Tenant admin

The `--api` server authenticates every request with a bearer token tied to a tenant identity. Tenants are managed from the CLI; the same identities are also exposed under `/v1/admin/tenants/*` when you'd rather drive provisioning over HTTP.

All five commands operate on `~/.arbiter/tenants.db` (a SQLite file, opened single-writer). They are CLI-only — no `--api` server has to be running.

## `--add-tenant <name>`

Provision a new tenant and print a fresh bearer token.

```
$ arbiter --add-tenant acme
Created tenant #3 (acme)

  API key (save this — it will not be shown again):
    atr_e8b1c4d5e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9

  Clients call:
    curl -N -H "Authorization: Bearer atr_..." \
         -H "Content-Type: application/json" \
         -d '{"agent":"index","message":"..."}' \
         http://<host>:<port>/v1/orchestrate
```

The plaintext token is **only visible at provisioning time** (and again if you rotate). Arbiter stores its SHA-256 digest, never the token itself. Lose it and rotate: `arbiter --rotate-tenant-token <id|name>`.

`<name>` is a free-form label for your reference (logs, audit). It doesn't have to be unique, but reusing names makes auditing harder. The id (sequential integer) is the stable identifier.

## `--list-tenants`

Print the tenant table.

```
$ arbiter --list-tenants
ID   Name                Status      Last used
------------------------------------------------------------
1    test                active      2026-04-30 14:55 UTC
2    acme                active      2026-05-02 09:12 UTC
3    legacy              disabled    2025-11-08 21:03 UTC
```

`Last used` is the timestamp of the most recent authenticated request from that tenant; `never` for tenants that have been provisioned but haven't called the API yet. Disabled tenants are kept in the table so audit trails stay intact — they just can't authenticate.

## `--rotate-tenant-token <id|name>`

Issue a new API key for an existing tenant and invalidate the previous digest immediately. Use this when upgrading from single-tenant (no-bearer) mode — those installs may have a tenant row whose plaintext was never shown — or whenever a key is lost or leaked.

```
$ arbiter --rotate-tenant-token acme
Rotated API key for tenant #2 (acme)

  API key (save this — it will not be shown again):
    atr_…

  Previous key is invalid immediately for new requests.

  NOTE: This CLI command updates the database only.  It cannot cancel()
  in-flight streams inside a running `arbiter --api` process.  For a hot
  revoke that stops outstanding work immediately, use
  POST /v1/admin/tenants/:id/rotate-token or
  PATCH /v1/admin/tenants/:id with {"disabled":true}.
```

HTTP equivalent: `POST /v1/admin/tenants/:id/rotate-token` (admin bearer). That path also cancels in-flight orchestrations for the tenant.

## `--disable-tenant <id|name>`

Revoke a tenant's access.

```
$ arbiter --disable-tenant acme
Disabled tenant 'acme'.

  NOTE: This CLI command updates the database only.  It cannot cancel()
  in-flight streams inside a running `arbiter --api` process.  For a hot
  revoke that stops outstanding work immediately, use
  PATCH /v1/admin/tenants/:id with {"disabled":true}.
```

Either the numeric id or the name works. Disabled tenants:

- Fail authentication on every endpoint (401).
- Keep their conversations, artifacts, memory entries, and scratchpads intact in the store.
- Do not have their tokens revoked at the cryptographic level — the digest is still in the DB. Re-enabling restores access with the same token.
- Stop mid-request work at the next `TenantGate` preflight (provider I/O boundary). Unlike admin HTTP disable, this path does **not** call `Orchestrator::cancel()` on in-flight streams — the CLI and `--api` are separate processes.

HTTP equivalent: `PATCH /v1/admin/tenants/:id` with `{"disabled":true}` (admin bearer). That path also cancels in-flight orchestrations for the tenant.

If you need to *invalidate* the token (irrecoverably) without creating a new tenant row, use `--rotate-tenant-token`.

## `--enable-tenant <id|name>`

Restore a previously-disabled tenant.

```
$ arbiter --enable-tenant acme
Enabled tenant 'acme'.
```

Symmetric with `--disable-tenant`. The original token is valid again.

## Resolution rules

For commands taking `<id|name>`:

- An all-digits argument is interpreted as a numeric id.
- Anything else is matched against the name field.
- If no tenant matches, the command exits `1` with `No tenant matched '<arg>'.`

Names are matched exactly (case-sensitive). Two tenants with the same name produce ambiguous results — you'll have to use the numeric id.

## Where this data lives

`~/.arbiter/tenants.db` — single SQLite file. Schema is internal; don't poke at it directly. Backing it up is straightforward (it's a normal SQLite file, copy while no `--api` server is running for a clean snapshot).

## See also

- [`docs/concepts/authentication.md`](../concepts/authentication.md) — how the server validates the token on the request path.
- [`docs/api/admin/tenants-create.md`](../api/admin/tenants-create.md) and friends — the same operations exposed over HTTP, gated by the admin token.
- [api.md](api.md) — the server that consumes these tokens.
