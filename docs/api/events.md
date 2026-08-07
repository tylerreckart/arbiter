# `POST /v1/events`

**Auth:** tenant — _Status:_ stable

Turns a structured hardware or software event into a full Arbiter run. The runtime routes the event to an agent, supplies that agent with its normal memory and tools, and streams the resulting reasoning and actions as Server-Sent Events.

Use this endpoint for application webhooks, infrastructure alerts, sensor readings, edge-device signals, robotics bridges, and other systems that produce events rather than conversational prompts.

## Request

### Body

| Field | Type | Required | Description |
|---|---|---|---|
| `type` | string | yes | Event type used for agent routing, such as `sensor.temperature.threshold` or `deployment.failed`. |
| `source` | string | no | Human-readable source identifier, such as `edge/rack-04` or `github/acme/api`. |
| `payload` | any JSON value | no | Event-specific data supplied to the selected agent. |
| `agent` | string | no | Explicit agent id. When present, bypasses type-based routing. |

### Headers

| Header | Required | Purpose |
|---|---|---|
| `Authorization` | yes | `Bearer <tenant token>`. See [authentication](../concepts/authentication.md). |
| `Content-Type` | yes | `application/json`. |

```bash
curl -N http://127.0.0.1:8080/v1/events \
  -H "Authorization: Bearer $ARBITER_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "type": "sensor.temperature.threshold",
    "source": "edge/rack-04",
    "payload": { "celsius": 84.6 }
  }'
```

## Routing

Agents opt into events with `event_types` in their constitution. Each entry is a glob matched against the event type (`fnmatch`, so `sensor.*` matches `sensor.temp.high`).

Routing order:

1. Explicit `agent` in the request body (any file-backed or tenant-stored id).
2. File-backed agents under the configured agents directory (`~/.arbiter/agents/*.json`).
3. Tenant agents created through [`POST /v1/agents`](agents/create.md), scanned in ascending `agent_id` order (full catalog, not the newest-200 REST list page).
4. If nothing matches, Arbiter routes the event to `index`.

```json
{
  "name": "facilities",
  "model": "ollama/qwen3.6",
  "event_types": [
    "sensor.*",
    "facility.alert.*"
  ],
  "capabilities": ["exec"]
}
```

Keep routing patterns distinct. If multiple agents in the same tier match an event, the first match in scan order wins (directory iteration for files; sorted `agent_id` for tenant agents).

## What the agent receives

Arbiter presents the event to the selected agent as a normal turn:

```text
Event: sensor.temperature.threshold
Source: edge/rack-04
Payload: {"celsius":84.6}
```

The selected agent can use the same memory, delegation, MCP, artifact, search, and permitted execution capabilities available to a direct orchestration request.

Event payloads are input data, but they are visible to the model. Treat event sources as untrusted, grant each routed agent only the capabilities it requires, and leave host execution disabled for externally sourced events. Prefer the tenant sandbox when an agent must execute commands.

## Response

The response is `text/event-stream` and follows the same lifecycle as [`POST /v1/orchestrate`](orchestrate.md): `request_received`, agent and tool activity, advisor decisions, and a terminal `done` event.

## Failure modes

| Status | When | Body |
|---|---|---|
| 400 | Body is not a JSON object, JSON is invalid, or `type` is missing. | `{"error":"..."}` |
| 401 | Bearer token is missing or invalid, or the tenant is disabled. | `{"error":"..."}` |
| 200 + `done.ok = false` | The routed run fails after the SSE stream opens. | SSE `error` followed by `done`. |

## See also

- [`POST /v1/orchestrate`](orchestrate.md) — direct request ingestion.
- [Agent data model](../concepts/data-model.md) — agent constitution fields.
- [SSE event catalog](../concepts/sse-events.md) — streamed event shapes.
- [Authentication](../concepts/authentication.md) — tenant bearer tokens.
