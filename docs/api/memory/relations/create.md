# `POST /v1/memory/relations`

**Auth:** tenant — _Status:_ stable

Create a directed labeled edge between two entries. Both HTTP callers and agents write directly into the curated graph — see [Structured memory](../../concepts/structured-memory.md).

Relations are **directed and per-type** — the same pair can have multiple relations of different kinds; the same `(source, target, relation)` triple cannot exist twice. Symmetric relations like `contradicts` are still stored directed; clients dedupe for display.

## Request

### Body

| Field       | Type          | Required | Constraint |
|-------------|---------------|----------|------------|
| `source_id` | int           | yes | Entry id this edge points **from**. Must belong to caller's tenant. |
| `target_id` | int           | yes | Entry id this edge points **to**. Must belong to caller's tenant. Must differ from `source_id` (no self-loops). |
| `relation`  | string (enum) | yes | One of `relates_to`, `refines`, `contradicts`, `supersedes`, `supports`. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"source_id":42,"target_id":43,"relation":"supports"}' \
  http://arbiter.example.com/v1/memory/relations
```

## Response

### 201 Created

```json
{
  "id": 7,
  "tenant_id": 1,
  "source_id": 42,
  "target_id": 43,
  "relation": "supports",
  "created_at": 1777058500
}
```

Field schemas: [Data model → MemoryRelation](../../concepts/data-model.md#memoryrelation).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; `source_id == target_id`; `relation` not in enum; either endpoint missing or belongs to another tenant. | `{"error": "..."}` (e.g. `"self-loops not allowed"`, `"entries belong to different tenants"`) |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 409    | Duplicate `(source_id, target_id, relation)` triple. | `{"error": "relation already exists", "existing_id": N}` |

## See also

- [`GET /v1/memory/relations`](list.md), [`DELETE /v1/memory/relations/:id`](delete.md).
- [Structured memory](../../concepts/structured-memory.md).
