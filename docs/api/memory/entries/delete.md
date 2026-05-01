# `DELETE /v1/memory/entries/:id`

**Auth:** tenant — _Status:_ stable

Permanently delete an entry. **Cascades to relations** with this entry as either endpoint.

For "this fact is no longer true but I want to keep the audit trail," use [`POST /v1/memory/entries/:id/invalidate`](invalidate.md) instead — it stamps `valid_to` rather than removing the row, leaves relations intact, and keeps the entry reachable through historical reads. See [Structured memory → Temporal model](../../concepts/structured-memory.md#temporal-model).

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

- [`POST /v1/memory/entries/:id/invalidate`](invalidate.md) — soft-delete with audit trail.
- [`PATCH /v1/memory/entries/:id`](patch.md).
- [Structured memory](../../concepts/structured-memory.md).
