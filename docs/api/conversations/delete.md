# `DELETE /v1/conversations/:id`

**Auth:** tenant — _Status:_ stable

Permanently delete a conversation and **all its messages, artifacts, and memory-entry artifact links**:

- Messages cascade-delete via FK.
- Artifacts cascade-delete via FK.
- Memory entries that referenced any of those artifacts have their `artifact_id` nullified by trigger (the entry survives; the link is cleared).

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Conversation id. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversations/1
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
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "conversation not found"}` |

## See also

- [`PATCH /v1/conversations/:id`](patch.md) — set `archived: true` to hide without deleting.
- [Artifacts](../concepts/artifacts.md) — for the cascade behaviour.
- [Structured memory](../concepts/structured-memory.md) — for the `artifact_id` clearing trigger.
