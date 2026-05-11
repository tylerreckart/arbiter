# `GET /v1/admin/audit`

**Auth:** admin — _Status:_ stable

Append-only log of every mutation issued through `/v1/admin/*`. Each row records who acted, what action, on which target, and a JSON snapshot of the state before and after. Used by operators investigating "who disabled this tenant?" / "when did that account get created?" without having to spelunk through stderr logs.

The log is **never** edited or deleted by the runtime. Operators wanting retention windows should periodically prune rows older than their policy directly in `~/.arbiter/tenants.db`.

## Request

| Query param  | Type | Required | Description |
|--------------|------|----------|-------------|
| `before_id`  | int  | no       | Newest cursor for backward pagination. Pass the smallest `id` from the previous page; omit (or `0`) for the latest. |
| `limit`      | int  | no       | Page size. Default `50`, hard cap `200`. |

Bearer admin token in `Authorization`.

```bash
curl \
  -H "Authorization: Bearer $ARBITER_ADMIN_TOKEN" \
  'http://arbiter.example.com/v1/admin/audit?limit=20'
```

## Response

### 200 OK

```json
{
  "entries": [
    {
      "id": 17,
      "ts": 1715472384,
      "actor": "admin",
      "action": "update_tenant",
      "target_kind": "tenant",
      "target_id": "5",
      "before": { "disabled": false },
      "after":  { "disabled": true }
    },
    {
      "id": 16,
      "ts": 1715471823,
      "actor": "admin",
      "action": "create_tenant",
      "target_kind": "tenant",
      "target_id": "5",
      "before": null,
      "after":  { "id": 5, "name": "acme" }
    }
  ]
}
```

Entries are returned newest-first by `id`. `ts` is epoch seconds. `before` and `after` are parsed JSON (or `null` when absent — `before` is null for create, `after` for delete). Token plaintext is **deliberately not** captured on `create_tenant` — losing the audit log shouldn't be equivalent to losing the credentials.

### Pagination

```
GET /v1/admin/audit?limit=50              # newest 50
GET /v1/admin/audit?before_id=23&limit=50  # next 50, older than id=23
```

## Action taxonomy (v1)

| `action`         | `target_kind` | `target_id`     | `before` / `after` shape |
|------------------|---------------|-----------------|--------------------------|
| `create_tenant`  | `tenant`      | new tenant id   | `before=null`; `after={id, name}` (token plaintext excluded). |
| `update_tenant`  | `tenant`      | tenant id       | `before={disabled}`; `after={disabled}`. |

Future actions land here as new fields without changing the response envelope. Consumers should treat unknown `action` values as opaque rather than erroring.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 200    | Normal. May return `{"entries": []}` for an empty log or a `before_id` past the oldest row. | JSON. |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 405    | Non-GET method. | `{"error": "method not allowed"}` |
| 503    | Server has no admin token configured (`~/.arbiter/admin_token` empty / unset). | `{"error": "admin endpoints disabled (...)"}` |

## See also

- [Operational notes](../concepts/operations.md) — broader admin / observability surface.
- [`PATCH /v1/admin/tenants/:id`](tenants-patch.md) — produces `update_tenant` audit rows.
- [`POST /v1/admin/tenants`](tenants-create.md) — produces `create_tenant` audit rows.
