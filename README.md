# Arbiter

A reasoning runtime for software, devices, and machines.

Arbiter turns prompts and events into supervised, stateful actions. Route
work to specialized agents, consult durable memory, execute permitted
tools, and observe every decision as a live stream of structured SSE.

It runs in three shapes.

- `arbiter` — interactive terminal client. Multi-pane TUI, persistent
  per-cwd session.
- `arbiter --api` — multi-tenant HTTP+SSE server. Ingest events and direct
  requests via `/v1/events` and `/v1/orchestrate`. OpenAPI 3.1,
  Agent2Agent (A2A) v1.0 compatible.
- `arbiter --send <agent> <message>` — one-shot dispatch for scripts and
  cron jobs.

## What it does

**Orchestrate any task.** Send a prompt from the TUI, the one-shot CLI,
or the HTTP API. The runtime fans work out to specialist agents, consults
durable memory, and streams the full reasoning trace back to the caller.

**Route events from software and hardware.** Turn webhooks, queues,
incidents, sensors, and edge devices into supervised actions — each event
type routed to the right agent with no custom glue code.

**Let it act — with supervision.** Constrain the tools each agent can use
and enforce advisor gates before consequential actions leave the runtime.

## Example sessions

All examples share the same stream shape. `/v1/orchestrate` takes a
direct prompt; `/v1/events` routes structured events from software or
hardware sources. Either way the full reasoning trace — delegation, tool
calls, memory, gates — streams back as SSE.

**Direct orchestration** — a research prompt dispatches parallel specialist
agents then hands their findings to a writer:

```
POST /v1/orchestrate
agent=index · "write a report on Neanderthal gene flow into modern humans"
─────────────────────────────────────────────────────────────────
event:   request_received     req_7p2n
event:   stream_start         index · depth 1

index: dispatching parallel research; writer will synthesize

event:   tool_call            /parallel genomics, archaeology, population-genetics
event:   delegate             genomics, archaeology, population-genetics
event:   stream_start         genomics · depth 2
event:   tool_call            /search Neanderthal introgression modern human genome
event:   tool_output          top results: Green et al. 2010, Prüfer et al. 2014
event:   tool_call            /fetch https://doi.org/10.1126/science.1188021
event:   tool_output          1–4% Neanderthal ancestry in non-African populations

genomics: adaptive alleles confirmed in STAT2, BNC2, OCA2

event:   stream_start         archaeology · depth 2
event:   tool_call            /search Neanderthal modern human coexistence fossil sites
event:   tool_output          key sites: Peștera cu Oase, Bacho Kiro, Zlatý kůň
event:   tool_call            /search Châtelperronian transition Europe 40000 BP
event:   tool_output          coexistence window ~50–40 ka; Oase 1 ~6–9% ancestry

archaeology: overlap concentrated in Europe and western Asia

event:   stream_start         population-genetics · depth 2
event:   tool_call            /search Neanderthal introgression direction asymmetry
event:   tool_output          Vernot & Akey 2014; Sankararaman et al. 2016
event:   tool_call            /search Neanderthal desert low-introgression regions
event:   tool_output          purifying selection reduced introgression near coding genes

population-genetics: asymmetric flow; immunity and pigmentation pathways retained

event:   join                 genomics, archaeology, population-genetics · ordered merge
event:   tool_call            /agent writer
event:   stream_start         writer · depth 2

writer: composing report from research findings

event:   tool_call            /write neanderthal-gene-flow.md
event:   gate                 verdict: continue ✓

writer: report complete — 1,840 words

event:   artifact             neanderthal-gene-flow.md · 1,840 words
event:   gate                 final review
event:   gate                 verdict: continue ✓
event:   done                 ok=true · 28.4s · $0.038
```

**Software event** — a GitHub PR webhook routes to a code review agent.
It fetches the diff, checks it against conventions stored in memory, and
produces a review artifact:

```
POST /v1/events · source=github/acme/api
type=github.pull_request.opened · pr=481 · author=jsmith
─────────────────────────────────────────────────────────────────
event:   event_received       evt_5r8q · authenticated service
event:   route_match          github.pull_request.* → reviewer
event:   stream_start         reviewer · depth 1

reviewer: fetching diff and checking against team conventions

event:   tool_call            /fetch https://github.com/acme/api/pull/481.diff
event:   tool_output          +312 −47 · 6 files · auth middleware, session handling
event:   tool_call            /mem read acme/api review conventions
event:   tool_output          require tests for auth changes · no direct session mutation
event:   tool_call            /search OWASP session fixation
event:   tool_output          session ID must be rotated on privilege change

reviewer: session ID not rotated after login — flags against convention and OWASP guidance

event:   tool_call            /write review-pr-481.md
event:   gate                 verdict: continue ✓
event:   tool_call            /mem write pr-481 review summary
event:   tool_output          memory mem_7c3k saved

reviewer: review complete — 1 blocking finding, 2 suggestions

event:   artifact             review-pr-481.md
event:   gate                 final review
event:   gate                 verdict: continue ✓
event:   done                 ok=true · 4.2s · action recorded
```

**Hardware event** — a temperature threshold breach on an edge device
routes to the facilities agent, which delegates parallel diagnostics and
operations sub-agents:

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

Every step is visible on the wire: routing, delegation, tool calls, memory
reads, gate verdicts, and the final outcome. Nothing happens off-stream.

Arbiter is experimental. The event surface, agent constitutions, and HTTP
shape are subject to change. `/exec` is unsandboxed; treat it accordingly.


## Documentation

- [`docs/getting-started/`](docs/getting-started/index.md) — quickstart paths
  to first agent reply.
- [`docs/philosophy.md`](docs/philosophy.md) — design philosophy: the six
  themes that explain why arbiter is shaped the way it is.
- [`docs/api/`](docs/api/index.md) — full HTTP API reference: concept
  pages (tenants, auth, SSE events, fleet streaming, MCP, A2A protocol,
  artifacts, structured memory, operations) and one page per endpoint.
- [`docs/cli/`](docs/cli/index.md) — non-interactive command-line
  reference: `--init`, `--send`, `--api`, tenant admin, environment
  variables.
- [`docs/tui/`](docs/tui/index.md) — interactive terminal client:
  screen anatomy, slash commands, keybindings, multi-pane layouts,
  streaming and turn lifecycle, session persistence.
- [`CHANGELOG.md`](CHANGELOG.md) — what changed, when. Breaking
  changes are flagged.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — build, tests, PR conventions.
- [`SECURITY.md`](SECURITY.md) — disclosure path for security
  vulnerabilities and operator hardening notes.


## Quick start

Full guide: [`docs/getting-started/`](docs/getting-started/index.md). Bare minimum for a local install:

```bash
# Install (macOS arm64)
curl -L https://github.com/tylerreckart/arbiter/releases/latest/download/arbiter-macos-arm64.tar.gz \
  | tar xz -C /usr/local/bin

# Configure (one provider key minimum)
export ANTHROPIC_API_KEY="sk-ant-..."

# Run
arbiter --init   # seed ~/.arbiter/ with starter agents
arbiter          # launch the terminal client
```

Linux binary, source builds, OpenAI/Ollama keys, the API server, and one-shot mode are all in [getting-started/local](docs/getting-started/local.md).


## Examples

**[Newton](https://github.com/tylerreckart/newton)** — SwiftUI iOS app that wraps the arbiter HTTP+SSE API as a reference client. Points at any `arbiter --api` instance and demonstrates how to drive the runtime end-to-end from a mobile frontend: bearer-token auth, streaming responses parsed event-by-event, conversation persistence against `/v1/conversations`, and rendering the tool-call surface as inline UI. Useful as a starting point if you're building your own frontend on top of arbiter.

**[3bo](examples/3bo/)** — small stationary robot that uses arbiter as its reasoning layer. An Arduino Nano ESP32 handles the physical interface: I2S microphone capture, wake-word detection, LED states, and speaker playback. A Jetson Orin Nano runs whisper.cpp for STT, Piper for TTS, and arbiter for reasoning. The firmware holds only a per-device bridge secret; cloud provider keys and arbiter tokens never leave the Jetson. A reference build for connecting hardware to arbiter over a local voice bridge.

Licensed under the [Apache License 2.0](LICENSE).
