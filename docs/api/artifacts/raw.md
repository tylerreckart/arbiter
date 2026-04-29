# `GET /v1/artifacts/:aid/raw`

**Auth:** tenant — _Status:_ stable

Tenant-scoped raw content blob with `Content-Type` + strong `ETag`. Same semantics as the conversation-scoped variant; cross-tenant ids surface as 404.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `aid`      | int  | Artifact id. |

### Conditional headers

| Header          | Description |
|-----------------|-------------|
| `If-None-Match` | If the value matches the artifact's ETag, respond `304` with no body. |

```bash
curl -H "Authorization: Bearer atr_…" \
  -H 'If-None-Match: "ad14a...e3"' \
  http://arbiter.example.com/v1/artifacts/12/raw
```

## Response

### 200 OK

Raw bytes; headers identical to the conversation-scoped variant. See [`GET /v1/conversations/:id/artifacts/:aid/raw`](conversations-raw.md).

### 304 Not Modified

When `If-None-Match` matches.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "artifact not found"}` |

## See also

- [`GET /v1/artifacts/:aid`](get.md) — metadata only.
- [`GET /v1/conversations/:id/artifacts/:aid/raw`](conversations-raw.md).
