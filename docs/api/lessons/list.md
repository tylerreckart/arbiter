# `GET /v1/lessons`

**Auth:** tenant — _Status:_ stable

List the calling tenant's lessons. Newest `last_seen_at` first by default; with a `q` parameter, ranked by `hit_count DESC` then `last_seen_at DESC`. Hard-capped at 200.

## Request

### Query parameters

| Param      | Description |
|------------|-------------|
| `agent_id` | Filter to one agent's lessons. Omit for the tenant's full set. |
| `q`        | Substring match against signature OR lesson_text (case-insensitive). |
| `limit`    | Cap; max 200 for browse, 50 for search. |

```bash
curl -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/lessons?agent_id=research&q=fetch"
```

## Response

### 200 OK

```json
{
  "lessons": [
    {
      "id": 14,
      "agent_id": "research",
      "signature": "/fetch behind cloudflare",
      "lesson_text": "use Authorization-bearing fetch, not plain GET",
      "hit_count": 7,
      "created_at": 1746720000,
      "updated_at": 1746720000,
      "last_seen_at": 1746810000
    }
  ]
}
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |

## See also

- [`POST /v1/lessons`](create.md), [`GET /v1/lessons/:id`](get.md), [`PATCH /v1/lessons/:id`](patch.md)
- [Lessons concept](../../concepts/lessons.md)
