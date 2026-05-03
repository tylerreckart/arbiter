# Tenant admin

The `--api` server authenticates every request with a bearer token tied to a tenant identity. Tenants are managed from the CLI; the same identities are also exposed under `/v1/admin/tenants/*` when you'd rather drive provisioning over HTTP.

All four commands operate on `~/.arbiter/tenants.db` (a SQLite file, opened single-writer). They are CLI-only — no `--api` server has to be running.

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

The plaintext token is **only visible at provisioning time**. Arbiter stores its SHA-256 digest, never the token itself. Lose it and you re-provision: there's no recovery path.

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

## `--disable-tenant <id|name>`

Revoke a tenant's access.

```
$ arbiter --disable-tenant acme
Disabled tenant 'acme'.
```

Either the numeric id or the name works. Disabled tenants:

- Fail authentication on every endpoint (401).
- Keep their conversations, artifacts, memory entries, and scratchpads intact in the store.
- Do not have their tokens revoked at the cryptographic level — the digest is still in the DB. Re-enabling restores access with the same token.

If you need to *invalidate* the token (irrecoverably), disable the tenant and provision a new one. There's no `--rotate-token`.

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

- [`docs/api/concepts/authentication.md`](../api/concepts/authentication.md) — how the server validates the token on the request path.
- [`docs/api/admin/tenants-create.md`](../api/admin/tenants-create.md) and friends — the same operations exposed over HTTP, gated by the admin token.
- [api.md](api.md) — the server that consumes these tokens.
