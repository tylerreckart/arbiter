# `GET /v1/lessons/:id`

**Auth:** tenant — _Status:_ stable

Fetch one lesson by id. Tenant-scoped: a leaked id never surfaces another tenant's row.

```bash
curl -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/lessons/14
```

## Response

### 200 OK

Same `lesson` shape as [`POST /v1/lessons`](create.md).

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 404    | Lesson not found for this tenant. |

## See also

- [`PATCH /v1/lessons/:id`](patch.md), [`DELETE /v1/lessons/:id`](delete.md), [`GET /v1/lessons`](list.md)
- [Lessons concept](../../concepts/lessons.md)
