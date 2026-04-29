# `DELETE /v1/artifacts/:aid`

**Auth:** tenant — _Status:_ stable

Tenant-scoped delete. Same semantics as the conversation-scoped variant — memory entries referencing this artifact have their link nullified by trigger.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `aid`      | int  | Artifact id. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/artifacts/12
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
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "artifact not found"}` |

## See also

- [`DELETE /v1/conversations/:id/artifacts/:aid`](conversations-delete.md).
- [Artifacts → Memory ↔ artifact linkage](../concepts/artifacts.md#memory--artifact-linkage).
