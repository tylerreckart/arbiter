# `PATCH /v1/memory/entries/:id`

**Auth:** tenant — _Status:_ stable

Update any subset of `{title, content, source, tags, type, status, artifact_id}`. `created_at` is immutable; `updated_at` is bumped on a successful change.

The `status` field is the **proposal-queue acceptance lever** — sending `"accepted"` on a `status="proposed"` entry promotes it into the curated graph. See [Structured memory → Proposal queue](../../concepts/structured-memory.md#proposal-queue).

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
| `status`      | string        | Only `"accepted"` is valid (promote a proposal). Demoting an accepted entry is not supported. |
| `artifact_id` | int \| null   | Set / clear the artifact link. `null` clears; positive int sets (validated against tenant catalog). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"status":"accepted"}' \
  http://arbiter.example.com/v1/memory/entries/42
```

## Response

### 200 OK

The updated `Entry` (hydrated `artifact` block if linked).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; field constraint violated; `status` not `"accepted"`; `artifact_id` doesn't exist for this tenant. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id not found, or belongs to another tenant. | `{"error": "entry not found"}` |
| 409    | `status="accepted"` PATCH on an already-accepted entry. | `{"error": "entry is not in 'proposed' status — nothing to accept."}` |
| 410    | Race: row vanished between accept and re-fetch (concurrent DELETE). | `{"error": "entry vanished between accept and re-fetch — refresh and retry"}` |

## See also

- [`DELETE /v1/memory/entries/:id`](delete.md) — the rejection path for a proposed entry.
- [Structured memory → Proposal queue](../../concepts/structured-memory.md#proposal-queue).
