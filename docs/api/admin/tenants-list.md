# `GET /v1/admin/tenants`

**Auth:** admin — _Status:_ stable

List all tenants. Used by the admin dashboard.

## Request

No path / query / body params.

```bash
curl -H "Authorization: Bearer adm_…" \
  http://arbiter.example.com/v1/admin/tenants
```

## Response

### 200 OK

```json
{
  "tenants": [
    {
      "id": 1,
      "name": "acme",
      "disabled": false,
      "created_at": 1777056438,
      "last_used_at": 1777078022
    }
  ]
}
```

Field schemas: [Data model → Tenant](../concepts/data-model.md#tenant). Tokens are **not** returned (the DB only stores their digests).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid admin bearer; tenant token presented. | `{"error": "..."}` |
| 503    | Server has no admin token configured. | `{"error": "admin not configured"}` |

## See also

- [`POST /v1/admin/tenants`](tenants-create.md), [`GET /v1/admin/tenants/:id`](tenants-get.md), [`PATCH /v1/admin/tenants/:id`](tenants-patch.md).
- [Tenants](../concepts/tenants.md), [Authentication](../concepts/authentication.md).
