# `DELETE /v1/memory/entries/:id`

**Auth:** tenant — _Status:_ stable

Permanently delete an entry. **Cascades to relations** with this entry as either endpoint.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Entry id. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/entries/42
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
| 404    | Id not found or belongs to another tenant. | `{"error": "entry not found"}` |

## See also

- [`PATCH /v1/memory/entries/:id`](patch.md).
- [Structured memory](../../concepts/structured-memory.md).
