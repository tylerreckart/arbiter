# `GET /v1/notifications/stream`

**Auth:** tenant — _Status:_ stable

Long-lived Server-Sent Events stream pushing `notification` events as scheduled task runs reach terminal state. Scoped to the calling tenant — a leaked subscription only sees that tenant's events.

This is the push counterpart to [`GET /v1/runs?since=<epoch>`](../runs/list.md). Front-ends typically open the stream once on session start, render events as they arrive, and fall back to the runs endpoint with the last seen `started_at` if the connection drops.

## Request

```bash
curl -N \
     -H "Authorization: Bearer atr_…" \
     http://arbiter.example.com/v1/notifications/stream
```

## Response

`Content-Type: text/event-stream`. The stream opens with a hello frame and emits one `notification` block per terminal run. A `: heartbeat` comment every 30 seconds keeps reverse-proxy idle timeouts from killing the connection.

```
event: open
data: {"ok":true}

event: notification
data: {"kind":"run.started","task_id":17,"run_id":42,"agent_id":"reviewer",
       "status":"running","started_at":1746720000,"completed_at":0}

event: notification
data: {"kind":"run.completed","task_id":17,"run_id":42,"agent_id":"reviewer",
       "status":"succeeded","started_at":1746720000,"completed_at":1746720185,
       "result_summary":"Reviewed deploy 2f8e91. All green."}

: heartbeat

event: notification
data: {"kind":"run.failed","task_id":18,"run_id":43,"agent_id":"research",
       "status":"failed","started_at":1746720500,"completed_at":1746720601,
       "error_message":"upstream rate limit"}
```

### Event kinds

| `kind`           | When                                                                  |
|------------------|-----------------------------------------------------------------------|
| `run.started`    | Scheduler has created the run row and is about to invoke the agent.   |
| `run.completed`  | Agent returned successfully. `result_summary` carries the final reply (truncated at 4 KiB). |
| `run.failed`     | Agent threw, the orchestrator init failed, or the target agent is missing. `error_message` carries detail. |

### Reconnection

The bus does not buffer events for offline subscribers. After a disconnect, replay missed events with:

```bash
curl -H "Authorization: Bearer atr_…" \
     "http://arbiter.example.com/v1/runs?since=<last_seen_started_at>"
```

## Failure modes

| Status | When |
|--------|------|
| 401    | Missing / invalid bearer. |
| 405    | Wrong HTTP method (only GET is supported). |
| 503    | Notifications subsystem not initialized — only happens if the server is mid-shutdown. |

## See also

- [`GET /v1/runs`](../runs/list.md) — durable replay surface.
- [Scheduler concept](../../concepts/scheduler.md)
