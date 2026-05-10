# `POST /v1/lessons`

**Auth:** tenant — _Status:_ stable

Record a lesson. Most lesson capture flows through the [`/lesson` writ](../../concepts/writ.md); this HTTP shape is for front-ends that surface lessons in a side panel where the user can edit them directly.

## Request

### Body

| Field         | Type   | Required | Default   | Description |
|---------------|--------|----------|-----------|-------------|
| `signature`   | string | yes      | —         | Short trigger pattern (≤ 200 chars). Typically the tool or pattern that triggers the failure. |
| `lesson_text` | string | yes      | —         | Remediation body (≤ 4096 chars). |
| `agent_id`    | string | no       | `"index"` | Owner. Lessons follow the agent identity across conversations. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"signature":"/fetch behind cloudflare","lesson_text":"use Authorization-bearing fetch, not plain GET","agent_id":"research"}' \
  http://arbiter.example.com/v1/lessons
```

## Response

### 201 Created

```json
{
  "lesson": {
    "id": 14,
    "agent_id": "research",
    "signature": "/fetch behind cloudflare",
    "lesson_text": "use Authorization-bearing fetch, not plain GET",
    "hit_count": 0,
    "created_at": 1746720000,
    "updated_at": 1746720000,
    "last_seen_at": 1746720000
  }
}
```

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Missing `signature` or `lesson_text`; field over the size limit; invalid JSON. | `{"error":"..."}` |
| 401    | Missing / invalid bearer. | `{"error":"..."}` |

## See also

- [`GET /v1/lessons`](list.md), [`PATCH /v1/lessons/:id`](patch.md), [`DELETE /v1/lessons/:id`](delete.md)
- [Lessons concept](../../concepts/lessons.md)
