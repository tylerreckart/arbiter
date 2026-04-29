# `DELETE /v1/conversations/:id/artifacts/:aid`

**Auth:** tenant — _Status:_ stable

Permanently delete an artifact. Memory entries that referenced it have their `artifact_id` nullified by trigger (the entry survives; the link clears). Quota counters decrement immediately.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |
| `aid`      | int  | Artifact id. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversations/7/artifacts/12
```

## Response

### 200 OK

```json
{ "deleted": true }
```

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist for this tenant + conversation pair. | `{"error": "artifact not found"}` |

## See also

- [`DELETE /v1/artifacts/:aid`](delete.md) — tenant-scoped variant.
- [Artifacts → Memory ↔ artifact linkage](../concepts/artifacts.md#memory--artifact-linkage) — what happens to memory references.
