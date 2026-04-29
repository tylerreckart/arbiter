# `PATCH /v1/conversations/:id`

**Auth:** tenant — _Status:_ stable

Update a conversation's title and/or archived flag. Both fields optional — apply whichever are present.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |

### Body

| Field      | Type    | Description |
|------------|---------|-------------|
| `title`    | string  | New display title. |
| `archived` | boolean | Hide from default list views without deletion (the list endpoint still returns archived rows; clients filter). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"title":"Q3 planning (final)","archived":true}' \
  http://arbiter.example.com/v1/conversations/1
```

## Response

### 200 OK

The updated `Conversation` object. Field schemas: [Data model → Conversation](../concepts/data-model.md#conversation).

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Body isn't a JSON object. | `{"error": "..."}` |
| 401    | Missing / invalid bearer. | `{"error": "..."}` |
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "conversation not found"}` |

## See also

- [`POST /v1/conversations`](create.md), [`GET /v1/conversations/:id`](get.md), [`DELETE /v1/conversations/:id`](delete.md).
