# Arbiter

**The agent that runs anywhere.**

![Release Version Shield](https://img.shields.io/github/v/release/tylerreckart/arbiter)
![License Shield](https://img.shields.io/github/license/tylerreckart/arbiter)

One small binary. Compile once, run it on a laptop, a server, an edge box, or
a CI runner. Feed it prompts, webhooks, or events from firmware and sensors —
it reasons, delegates to specialist sub-agents, acts through a constrained
tool surface, and streams every step back as structured SSE.

![Arbiter session sidebar and inline diff rendering](assets/terminal.jpg)

## Run it anywhere

Same agent, three interfaces:

| Where | How |
|-------|-----|
| Locally | `arbiter` — interactive multi-pane TUI, persistent per-cwd sessions |
| A script or cron job | `arbiter --send <agent> "..."` — one-shot dispatch |
| Your stack | `arbiter --api` — HTTP+SSE server, tenant-isolated, speaks A2A v1.0 |
| A device or robot | `POST /v1/events` from firmware or a bridge — the device sends a request, arbiter does the reasoning |
| A webhook or queue | Declare a glob pattern per agent and arbiter routes matching events there — no dispatcher to write |

No runtime to install beyond the binary itself. Provider keys (OpenRouter,
Ollama, etc.) are the only external dependency for model calls.

## Built differently

Most agent frameworks bolt orchestration onto JSON tool-calling: the model
returns a structured call, the runtime executes it, the result comes back on
a dedicated turn boundary. Arbiter doesn't do that. Its agents speak
**writ** — a small DSL of slash prefixed commands (`/fetch`, `/exec`, `/agent`,
`/mem`, ...) emitted inline in the model's own prose. The runtime watches the
token stream as it's produced, dispatches each writ against a hard
per-agent allowlist, and feeds the result back on the next turn. No schema
negotiation, no separate tool-use protocol — capability gating happens at
dispatch time, not at parse time. See [`docs/concepts/writ.md`](docs/concepts/writ.md).

That same substrate is why supervision, memory, and federation aren't
separate systems bolted on top — they're the same DSL:

- **Structural supervision, not self-critique.** Configure an agent's
  advisor in `gate` mode and a second, higher-capability model reviews every
  terminating turn before the result leaves the runtime, returning
  `CONTINUE`, `REDIRECT`, or `HALT`. The executor cannot grade its own work.
- **Typed, temporal memory.** Facts live in a graph of entries and
  relations, retired with a validity window instead of deleted, retrieved
  through layered lexical → locality → semantic search. Agents recall across
  sessions instead of re-deriving the same conclusion every turn.
- **Federated by default.** `/mcp` reaches any stdio MCP server — Playwright,
  Stripe, or a hosted service proxied through `mcp-remote`. `/a2a` calls out
  to remote agents, and every local agent is itself reachable as an A2A v1.0
  peer, in both directions.
- **Delegation as a first-class primitive.** `/agent`, `/parallel`, and
  `/pane` make sub-agents callable the same way a tool is callable —
  synchronous, fanned out, or fire-and-forget — up to two levels deep, all
  multiplexed onto one event stream.

## What it does

**Orchestrate any task.** Send a prompt from the TUI, one-shot CLI, or HTTP
API. The runtime fans work out to specialist agents, consults durable
memory, and streams the full reasoning trace back to the caller.

**Route events from software and hardware.** Turn GitHub webhooks, incident
feeds, sensor readings, and edge telemetry into supervised agent runs.
Declare which event types an agent handles; arbiter matches and routes
without a hand-written dispatcher.

**Act with supervision.** Every agent's tool access is a hard allowlist, not
a suggestion in the system prompt. Layer an advisor gate on top and
consequential turns get a second model's sign-off before they reach you.

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
shape are subject to change. `/exec` is unsandboxed by default; treat it
accordingly.

Licensed under the [Apache License 2.0](LICENSE).
