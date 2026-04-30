# `GET /v1/memory/entries/:id`

**Auth:** tenant — _Status:_ stable

Read one entry, with the linked artifact hydrated inline if `artifact_id` is set.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Entry id. |

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/entries/42
```

## Response

### 200 OK

Same shape as the [POST response](create.md), with a nested `artifact` block when `artifact_id` is set. Field schemas: [Data model → MemoryEntry](../../concepts/data-model.md#memoryentry).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "entry not found"}` |

## See also

- [`PATCH /v1/memory/entries/:id`](patch.md), [`DELETE /v1/memory/entries/:id`](delete.md).
