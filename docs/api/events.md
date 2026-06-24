# `POST /v1/events`

**Auth:** tenant — _Status:_ experimental

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

File-backed agents opt into events with `event_types` in their constitution. Each entry is a glob matched against the event type. Arbiter scans the JSON definitions in its configured agents directory; tenant agents created through `POST /v1/agents` are not part of automatic event routing in this experimental version. If no file-backed agent matches, Arbiter routes the event to `index`.

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

An explicit `agent` in the request body takes precedence over `event_types` routing.

Keep routing patterns distinct. If multiple agents match an event in this experimental implementation, the first matching agent definition is selected.

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
