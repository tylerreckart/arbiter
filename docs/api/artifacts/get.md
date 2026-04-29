# `GET /v1/artifacts/:aid`

**Auth:** tenant — _Status:_ stable

Tenant-scoped lookup by artifact id — the conversation id is inferred from the row. Same semantics as the conversation-scoped variant; cross-tenant ids surface as 404 (never 403, to avoid id-existence side channels).

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `aid`      | int  | Artifact id. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/artifacts/12
```

## Response

### 200 OK

`ArtifactRecord` (no `content`). Field schemas: [Data model → ArtifactRecord](../concepts/data-model.md#artifactrecord).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "artifact not found"}` |

## See also

- [`GET /v1/artifacts/:aid/raw`](raw.md) — content blob.
- [`GET /v1/conversations/:id/artifacts/:aid`](conversations-get.md) — same semantics, conversation-scoped path.
