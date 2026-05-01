# `PATCH /v1/memory/entries/:id`

**Auth:** tenant — _Status:_ stable

Update any subset of `{title, content, source, tags, type, artifact_id}`. `created_at` is immutable; `updated_at` is bumped on a successful change.

**Active rows only.** Invalidated entries (those with a non-null `valid_to`) cannot be modified through this endpoint — historical records are immutable. To correct an invalidated entry, hard-delete it (`DELETE /v1/memory/entries/:id`) and re-create. See [Structured memory → Temporal model](../../concepts/structured-memory.md#temporal-model).

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Entry id. |

### Body

Any subset of:

| Field         | Type          | Constraint |
|---------------|---------------|------------|
| `title`       | string        | 1–200 chars. |
| `content`     | string        | ≤ 64 KiB. |
| `source`      | string        | ≤ 200 chars. |
| `tags`        | array<string> | 0–32 tags, each 1–64 chars. |
| `type`        | string (enum) | One of the six entry types. |
| `artifact_id` | int \| null   | Set / clear the artifact link. `null` clears; positive int sets (validated against tenant catalog). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"title":"Findings report (revised)"}' \
  http://arbiter.example.com/v1/memory/entries/42
```

## Response

### 200 OK

The updated `Entry` (hydrated `artifact` block if linked).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; field constraint violated; `artifact_id` doesn't exist for this tenant. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id not found, belongs to another tenant, or has been invalidated. | `{"error": "entry not found"}` |

## See also

- [`DELETE /v1/memory/entries/:id`](delete.md), [`POST /v1/memory/entries/:id/invalidate`](invalidate.md).
- [Structured memory](../../concepts/structured-memory.md).
