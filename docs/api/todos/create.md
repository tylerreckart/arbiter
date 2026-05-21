# `POST /v1/todos`

**Auth:** tenant — _Status:_ stable

Create a todo. Most agent-driven creation flows through the [`/todo` writ](../../concepts/writ.md); this HTTP shape is for front-ends that drive the surface directly (e.g. a side panel where a user types in their own progress checklist).

## Request

### Body

| Field             | Type   | Required | Default     | Description |
|-------------------|--------|----------|-------------|-------------|
| `subject`         | string | yes      | —           | Short title. |
| `description`     | string | no       | `""`        | Free-form details. |
| `agent_id`        | string | no       | `"index"`   | The owner of record. Captured at create time; not enforced (any tenant agent may later mark it done). |
| `conversation_id` | int    | no       | `0`         | Pin to a conversation thread. `0` ⇒ tenant-wide (visible from every thread). |
| `status`          | string | no       | `"pending"` | Seed status: `pending` \| `in_progress` \| `completed` \| `canceled`. Terminal values also stamp `completed_at = created_at` so historical rows don't look like in-flight work that just resolved. Useful when migrating from another tracker. |

```bash
curl -X POST \
  -H "Authorization: Bearer atr_…" \
  -H "Content-Type: application/json" \
  -d '{"subject":"review the deploy","description":"check logs + metrics","conversation_id":7}' \
  http://arbiter.example.com/v1/todos
```

## Response

### 201 Created

```json
{
  "todo": {
    "id": 14,
    "conversation_id": 7,
    "agent_id": "index",
    "subject": "review the deploy",
    "description": "check logs + metrics",
    "status": "pending",
    "position": 1,
    "created_at": 1746720000,
    "updated_at": 1746720000,
    "completed_at": 0
  }
}
```

`position` is `MAX(position) + 1` within the same `(tenant, conversation_id, status='pending')` bucket — newly created todos always sort to the bottom of pending until the agent (or a PATCH) reorders them.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400    | Missing `subject`; invalid `status`; invalid JSON. | `{"error":"..."}` |
| 401    | Missing / invalid bearer.        | `{"error":"..."}` |

## See also

- [`GET /v1/todos`](list.md), [`PATCH /v1/todos/:id`](patch.md), [`DELETE /v1/todos/:id`](delete.md)
- [Todos concept](../../concepts/todos.md)
