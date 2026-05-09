# `DELETE /v1/lessons/:id`

**Auth:** tenant — _Status:_ stable

Hard-delete a lesson. There's no archive — once deleted, it won't surface in pre-turn injection or search. Recreate the row to bring it back.

```bash
curl -X DELETE \
  -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/lessons/14
```

## Response

### 200 OK

```json
{ "deleted": true }
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Lesson not found for this tenant. |

## See also

- [Lessons concept](../../concepts/lessons.md)
