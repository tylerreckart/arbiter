# `DELETE /v1/conversation-folders/:id`

**Auth:** tenant — _Status:_ stable

Delete a conversation folder. Conversations in the folder are **unfiled** (`folder_id` cleared); they are not deleted.

## Request

| Path param | Type | Description |
|------------|------|-------------|
| `id`       | int  | Folder id. |

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/conversation-folders/3
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
| 404    | Id doesn't exist or belongs to another tenant. | `{"error": "folder not found"}` |

## See also

- [`GET /v1/conversation-folders`](list.md), [`POST /v1/conversation-folders`](create.md), [`PATCH /v1/conversation-folders/:id`](patch.md).
- [`PATCH /v1/conversations/:id`](../conversations/patch.md) — move a conversation with `folder_id`.
