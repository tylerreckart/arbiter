# `PATCH /v1/memory/relations/:id`

**Auth:** tenant — _Status:_ stable

Promote a proposed relation into the curated graph. Relations are otherwise immutable (no `title`/`content` to edit) — this endpoint exists *solely* for proposal acceptance.

**Both endpoint entries must already be accepted** before a relation can be accepted. The canonical reviewer flow for a multi-row proposal (entry A + entry B + edge A→B) is: accept A, accept B, then accept the edge.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Relation id. |

### Body

Exactly:

```json
{ "status": "accepted" }
```

Any other body shape returns `400`. To reject a proposed relation, [`DELETE`](delete.md) it.

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"status":"accepted"}' \
  http://arbiter.example.com/v1/memory/relations/12
```

## Response

### 200 OK

The updated `Relation` with `status: "accepted"`. Field schemas: [Data model → MemoryRelation](../../concepts/data-model.md#memoryrelation).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body shape wrong (anything other than `{"status":"accepted"}`). | `{"error": "the only supported PATCH on a relation is {\"status\": \"accepted\"} — reject by DELETE."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Relation id not found / wrong tenant. | `{"error": "relation not found"}` |
| 409    | Either endpoint entry is still in `status="proposed"`. | `{"error": "cannot accept relation — it must be in 'proposed' status and both endpoint entries must already be accepted. Accept the entry endpoints first, then retry."}` |
| 410    | Race: row vanished between accept and re-fetch. | `{"error": "relation vanished between accept and re-fetch — refresh and retry"}` |

## See also

- [`DELETE /v1/memory/relations/:id`](delete.md) — rejection path.
- [Structured memory → Proposal queue](../../concepts/structured-memory.md#proposal-queue).
