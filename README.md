# Arbiter

**The agent that runs anywhere.**

One small binary. Compile once and run it anywhere. laptop, server, edge box,
CI runner, microcontroller. Feed it prompts, webhooks, and sensor events. 
It reasons, delegates, acts, and streams every step back as structured SSE.

**In:** prompts · events · API calls · scheduled jobs  
**Out:** supervised actions · artifacts · memory · a live trace of every decision

## Run it anywhere

Same agent, three interfaces:

| Where | How |
|-------|-----|
| Locally | `arbiter` — interactive TUI, persistent per-cwd session |
| A script or cron job | `arbiter --send <agent> "..."` — one-shot dispatch |
| Your stack | `arbiter --api` — HTTP+SSE server; OpenAPI 3.1, A2A v1.0 |
| A device or robot | POST events to `/v1/events` from firmware or a bridge |
| A webhook or queue | Route structured events to the right agent — no glue code |

No runtime to install beyond the binary itself. Provider keys (OpenRouter,
Ollama, etc.) are the only external dependency for model calls.

## What it does

**Orchestrate any task.** Send a prompt from the TUI, one-shot CLI, or HTTP
API. The runtime fans work out to specialist agents, consults durable memory,
and streams the full reasoning trace back to the caller.

**Route events from software and hardware.** Turn GitHub webhooks, incident
feeds, sensor readings, and edge telemetry into supervised actions. Know each
event type is routed to the right agent.

**Act with supervision.** Constrain the tools each agent can use and enforce
advisor gates before consequential actions leave the runtime.

## Example session

`/v1/orchestrate` takes a direct prompt; `/v1/events` routes structured
events from software or hardware. Either way the full trace — routing,
delegation, tool calls, memory, gates — streams back as SSE. More examples
in the [API docs](https://arbiter.run/docs/api).

**Hardware event** — a temperature threshold on an edge device routes to a
facilities agent, which delegates parallel diagnostics and operations
sub-agents:

```
POST /v1/events · source=edge/rack-04
type=sensor.temperature.threshold · value=84.6°C
─────────────────────────────────────────────────────────────────
event:   event_received       evt_8s3z · authenticated device
event:   route_match          sensor.* → facilities
event:   stream_start         facilities · depth 1

facilities: checking thermal history and response policy

event:   tool_call            /mem read rack-04 thermal history
event:   tool_output          3 prior events · cooling policy v3
event:   tool_call            /parallel diagnostics, operations
event:   delegate             diagnostics, operations
event:   stream_start         diagnostics · depth 2
event:   tool_call            /exec read_sensors --zone rack-04
event:   tool_output          inlet=31.2 · cpu=84.6 · fan=61%
event:   tool_call            /exec inspect_processes --top 5
event:   tool_output          inference-worker using 93% GPU

diagnostics: sustained compute load is driving the thermal spike

event:   stream_start         operations · depth 2
event:   tool_call            /exec reduce_load --service inference-worker
event:   tool_output          load target 60% · graceful drain started
event:   gate                 verdict: continue ✓
event:   tool_call            /mem write thermal event + response
event:   join                 diagnostics, operations · ordered merge

facilities: rack-04 stabilized; monitoring for recurrence

event:   artifact             incident evt_8s3z · trace + actions
event:   gate                 final review · action + outcome
event:   gate                 verdict: continue ✓
event:   done                 ok=true · 6.8s · action recorded
```

Nothing happens off-stream.

## Quick start

```bash
# Install (macOS arm64)
curl -L https://github.com/tylerreckart/arbiter/releases/latest/download/arbiter-macos-arm64.tar.gz \
  | tar xz -C /usr/local/bin

# Configure hosted models through OpenRouter
export OPENROUTER_API_KEY="sk-or-..."

# Run
arbiter --init   # seed ~/.arbiter/ with starter agents
arbiter          # launch the terminal client
```

Linux binary, source builds, OpenRouter/Ollama setup, the API server, and
one-shot mode are in [getting-started/local](docs/getting-started/local.md).

## Documentation

- [`docs/getting-started`](https://arbiter.run/docs/getting-started/local) —
  quickstart paths to first agent reply.
- [`docs/philosophy`](https://arbiter.run/docs/philosophy) — design
  philosophy: the six themes that explain why arbiter is shaped the way it is.
- [`docs/api/`](https://arbiter.run/docs/api) — full HTTP API reference:
  tenants, auth, SSE events, fleet streaming, MCP, A2A, artifacts, memory,
  operations, and one page per endpoint.
- [`docs/cli/`](https://arbiter.run/docs/cli) — non-interactive command-line
  reference: `--init`, `--send`, `--api`, tenant admin, environment variables.
- [`docs/tui/`](https://arbiter.run/docs/tui) — interactive terminal client:
  screen anatomy, slash commands, keybindings, multi-pane layouts, streaming
  and turn lifecycle, session persistence.
- [`CHANGELOG.md`](CHANGELOG.md) — what changed, when. Breaking changes are
  flagged.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — build, tests, PR conventions.
- [`SECURITY.md`](SECURITY.md) — disclosure path for security
  vulnerabilities and operator hardening notes.

Arbiter is experimental. The event surface, agent constitutions, and HTTP
shape are subject to change. `/exec` is unsandboxed; treat it accordingly.

Licensed under the [Apache License 2.0](LICENSE).
