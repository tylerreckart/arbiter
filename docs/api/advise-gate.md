# `POST /v1/advise/gate`

**Auth:** tenant — _Status:_ beta

Stateless advisor-gate verdict. Same `run_advisor_gate()` the orchestrator uses on an executor's terminating turn. Does **not** own the executor loop — the caller must honour `CONTINUE` / `REDIRECT` / `HALT` itself.

Use this when an external framework drives its own agent loop and wants Arbiter's gate decision without a full `/v1/orchestrate` turn.

This route shares the per-tenant rate and concurrency limiter with orchestrate, events, agent chat, and A2A. Surplus requests return `429`.

## Request

### Body

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `advisor_model` | string | yes | — | Provider-prefixed model id for the gate call (e.g. `claude-opus-4-7`). |
| `original_task` | string | yes | — | The user's original task given to the executor. Capped at 32 KB. |
| `terminating_text` | string | yes | — | The executor's text for its terminating turn. Capped at 32 KB. |
| `tool_summary` | string | no | `""` | Pre-formatted tool-call summary, one line per call. Capped at 8 KB. |
| `prompt` | string | no | built-in gate prompt | System-prompt override. Capped at 16 KB. |

Oversized fields are truncated (with an `[truncated]` marker) before the advisor prompt is built — they are not rejected.

```bash
curl http://127.0.0.1:8080/v1/advise/gate \
  -H "Authorization: Bearer $ARBITER_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "advisor_model": "claude-opus-4-7",
    "original_task": "Summarize the attached RFC",
    "terminating_text": "The RFC proposes replacing the queue with a ring buffer.",
    "tool_summary": "- read args=rfc.md result=OK 12 KB"
  }'
```

### Headers

| Header | Required | Purpose |
|---|---|---|
| `Authorization` | yes | `Bearer <tenant token>`. See [authentication](../concepts/authentication.md). |
| `Content-Type` | yes | `application/json`. |

## Response

### 200

```json
{
  "signal": "CONTINUE",
  "text": "",
  "malformed": false
}
```

| Field | Meaning |
|-------|---------|
| `signal` | `CONTINUE` \| `REDIRECT` \| `HALT`. |
| `text` | Guidance (`REDIRECT`) or reason (`HALT`); empty for `CONTINUE`. |
| `malformed` | True when the advisor's reply was unparseable. This endpoint does **not** apply `malformed_halts` — the caller decides fail-closed vs fail-open. |

The gate is one history-less `complete()` on `advisor_model`. Transport or provider errors return `signal: "HALT"` with `malformed: true` and the error in `text`.

## Failure modes

| Status | When | Body |
|--------|------|------|
| 400 | Body is not a JSON object, or `advisor_model` / `original_task` / `terminating_text` is missing. | `{"error":"..."}` |
| 401 | Bearer token missing/invalid, or tenant disabled. | `{"error":"..."}` |
| 405 | Method is not POST. | plain text |
| 429 | Per-tenant rate or concurrency limiter rejected the call. | `{"error":"rate limit exceeded","reason":"rate_limit"\|"concurrent_request_limit","retry_after_seconds":N}` |
| 500 | Orchestrator / client init failed. | `{"error":"..."}` |

## See also

- [Advisor concept](../concepts/advisor.md) — signal grammar and in-loop enforcement
- [Operations](../concepts/operations.md#per-tenant-rate--concurrency-limiting) — limiter env vars
- [`POST /v1/orchestrate`](orchestrate.md) — full dispatch with the gate owned by the runtime
- [`POST /v1/intent`](intent.md) — pre-dispatch classify, not a post-turn gate
