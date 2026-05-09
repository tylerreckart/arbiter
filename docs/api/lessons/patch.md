# `PATCH /v1/lessons/:id`

**Auth:** tenant — _Status:_ stable

Update `signature` and/or `lesson_text`. Any field omitted is left untouched. `agent_id`, `hit_count`, and timestamps are not editable through this endpoint.

## Request

### Body

| Field         | Type   | Description |
|---------------|--------|-------------|
| `signature`   | string | New trigger pattern (≤ 200 chars). |
| `lesson_text` | string | New remediation body (≤ 4096 chars). |

```bash
curl -X PATCH \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"lesson_text":"use Authorization-bearing fetch with retry-after backoff"}' \
  http://arbiter.example.com/v1/lessons/14
```

## Response

### 200 OK

Returns the updated `lesson`.

## Failure modes

| Status | When |
|--------|------|
| 400    | No mutable fields supplied; field over size limit; invalid JSON. |
| 401    | Missing / invalid bearer. |
| 404    | Lesson not found for this tenant. |

## See also

- [`GET /v1/lessons/:id`](get.md), [`DELETE /v1/lessons/:id`](delete.md)
- [Lessons concept](../../concepts/lessons.md)
