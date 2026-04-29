# `GET /v1/conversations/:id/artifacts/:aid/raw`

**Auth:** tenant — _Status:_ stable

Raw content blob with `Content-Type` (from the artifact's `mime_type`) and a strong `ETag` (= the artifact's `sha256`, RFC 7232-quoted). Conditional `If-None-Match` returns `304 Not Modified` cheaply.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |
| `aid`      | int  | Artifact id. |

### Conditional headers

| Header           | Description |
|------------------|-------------|
| `If-None-Match`  | If the value matches the artifact's `ETag`, respond `304` with no body. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversations/7/artifacts/12/raw
```

## Response

### 200 OK

Raw bytes. Headers:

| Header           | Value |
|------------------|-------|
| `Content-Type`   | The artifact's `mime_type`. |
| `Content-Length` | Bytes. |
| `ETag`           | `"<sha256>"` (RFC 7232 strong validator). |

### 304 Not Modified

When `If-None-Match` matches. Empty body, includes the same `ETag`.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist for this tenant + conversation pair, or the row's content is missing (data corruption — defensive). | `{"error": "artifact not found"}` or `{"error": "artifact content missing"}` |

## See also

- [`GET /v1/conversations/:id/artifacts/:aid`](conversations-get.md) — metadata only.
- [`GET /v1/artifacts/:aid/raw`](raw.md) — tenant-scoped variant.
