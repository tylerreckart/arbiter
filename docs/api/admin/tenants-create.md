# `POST /v1/admin/tenants`

**Auth:** admin — _Status:_ stable

Create a tenant. **Returns the plaintext bearer token exactly once** — save it on creation; the database keeps only a SHA-256 digest. If a token is lost, issue a new one (delete-and-recreate, or via a future rotate endpoint).

## Request

### Body

| Field  | Type   | Required | Description |
|--------|--------|----------|-------------|
| `name` | string | yes      | Display name. No uniqueness constraint — pick your own convention. |

```bash
curl -X POST \
  -H "Authorization: Bearer adm_…" \
  -H "Content-Type: application/json" \
  -d '{"name":"acme"}' \
  http://arbiter.example.com/v1/admin/tenants
```

## Response

### 201 Created

```json
{
  "id": 3,
  "name": "acme",
  "disabled": false,
  "created_at": 1777056438,
  "last_used_at": 0,
  "token": "atr_6c4265a8cf89b44dca6bb50090975e9201ec990a91220017b63026efd54e1638"
}
```

The `token` field is the plaintext tenant token and is **only** returned here.

Caps, usage tracking, and invoicing are owned by the sibling [Quartermaster](../../../../quartermaster/docs/README.md) service when configured — provision the matching workspace there separately.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; missing `name`. | `{"error": "..."}` |
| 401    | Missing / invalid admin bearer. | `{"error": "..."}` |
| 503    | Server has no admin token configured. | `{"error": "admin not configured"}` |

## See also

- [`GET /v1/admin/tenants`](tenants-list.md), [`GET /v1/admin/tenants/:id`](tenants-get.md), [`PATCH /v1/admin/tenants/:id`](tenants-patch.md).
- [Tenants](../concepts/tenants.md).
