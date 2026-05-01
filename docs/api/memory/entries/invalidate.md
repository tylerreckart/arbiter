# `POST /v1/memory/entries/:id/invalidate`

**Auth:** tenant — _Status:_ stable

Soft-delete an entry by stamping its `valid_to` field. The row stays in the database and remains reachable through historical reads (`GET /v1/memory/entries?as_of=<epoch>`); it disappears from the default active set used by `/v1/memory/entries`, `/v1/memory/entries/:id`, and the agent-facing `/mem search` / `/mem entries`.

Distinct from `DELETE` — invalidation preserves the row for audit and replay. See [Structured memory → Temporal model](../../concepts/structured-memory.md#temporal-model) for the rationale.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Entry id. |

### Body

The body is optional. When present, it's a JSON object with one optional field:

| Field | Type | Description |
|-------|------|-------------|
| `when` | integer | Epoch seconds to stamp into `valid_to`. Default = wall-clock `now()`. Must be `≥ 0`. |

Pass an explicit `when` for replay scenarios (importing a fact that was already retired at a known historical moment). Most live invalidations omit it.

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/entries/42/invalidate
```

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"when": 1735689600}' \
  http://arbiter.example.com/v1/memory/entries/42/invalidate
```

## Response

### 200 OK

```json
{ "invalidated": true, "id": 42 }
```

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Invalid JSON; negative `when`. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist for this tenant *or* the row was already invalidated. The two cases collapse here because they're indistinguishable to a default reader (an invalidated row isn't visible via `GET /v1/memory/entries/:id`). | `{"error": "entry not found or already invalidated"}` |
| 409    | A concurrent request invalidated the row between the active-state probe and the UPDATE. Rare; benign — the after-state matches the caller's intent. | `{"error": "entry was already invalidated by a concurrent request"}` |

The merge of "not found" and "already invalidated" into a single `404` is intentional. The runtime doesn't expose a "fetch invalidated row by id" point read — that path would let callers trivially bypass the active-only filter; instead, historical reads happen via `as_of` on the list endpoint, which scopes the audit window deliberately.

## Idempotency

Invalidation is **one-directional and idempotent under repeated calls**. Once a row has `valid_to` set, subsequent invalidate requests return `404` without changing the timestamp. To "un-invalidate" a row, hard-delete it (`DELETE /v1/memory/entries/:id`) and re-create it with the desired content.

## Ordering vs. update_entry

`PATCH /v1/memory/entries/:id` (and the agent's `/mem add entry` repair path) only modify rows where `valid_to IS NULL`. An invalidated row is immutable through the normal write surface — historical content stays as it was at the moment of invalidation. To correct a historical record, hard-delete and re-create.

## Effect on relations

Memory relations whose source or target was invalidated **stay in the database**. They cascade-DELETE only when their endpoint is hard-deleted via `DELETE /v1/memory/entries/:id`. Consumers that want "active relations only" semantics should check both endpoint entries' `valid_to IS NULL` at read time.

## See also

- [`DELETE /v1/memory/entries/:id`](delete.md) — hard delete with relation CASCADE.
- [`GET /v1/memory/entries`](list.md) — historical reads via `?as_of=<epoch>`.
- [Structured memory → Temporal model](../../concepts/structured-memory.md#temporal-model) — rationale for soft-delete and the validity window contract.
