# `DELETE /v1/memory/relations/:id`

**Auth:** tenant — _Status:_ stable

Delete one edge. Works on both accepted and proposed rows; the latter is the **rejection path for a proposed relation**.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Relation id. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/memory/relations/7
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
| 404    | Id not found or belongs to another tenant. | `{"error": "relation not found"}` |

## See also

- [`POST /v1/memory/relations`](create.md), [`PATCH /v1/memory/relations/:id`](patch.md).
