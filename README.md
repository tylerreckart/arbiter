# Arbiter

**The agent that runs anywhere.**

![Release Version Shield](https://img.shields.io/github/v/release/tylerreckart/arbiter)
![License Shield](https://img.shields.io/github/license/tylerreckart/arbiter)

One small binary. Compile once and run it everywhere. A laptop, server, edge box,
CI runner, or microcontroller. Feed it prompts, webhooks, and sensor events. 
It reasons, delegates, acts, and streams every step back as structured SSE.

![Arbiter interactive TUI](assets/output.gif)

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
