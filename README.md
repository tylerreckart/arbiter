# Arbiter

A multi-agent orchestration runtime for the terminal and the network.

Arbiter runs a master agent named `index` that decides whether to answer a
request directly or delegate it to a specialist, streams the work back
live, and persists the durable output — files, structured memory, agent
definitions — across sessions. Agents drive the runtime with **writ**, a
small slash-command DSL they emit inline in their replies: fetch a URL,
run a shell command, write a file, record a memory, call each other.

It runs in three shapes.

- `arbiter` — interactive terminal client. Multi-pane TUI, persistent
  per-cwd session.
- `arbiter --api` — multi-tenant HTTP+SSE server with bearer-token auth.
  Speaks the arbiter-native protocol and the Agent2Agent (A2A) v1.0
  protocol — agents are reachable from any A2A-compatible client and
  can delegate to remote A2A agents.
- `arbiter --send <agent> <message>` — one-shot dispatch for scripts and
  cron jobs.

## Example session

```
> What's the canonical paper on Bayesian neural networks?

arbiter ↗ POST /v1/orchestrate
agent:                  index
message:                "What's the canonical paper on Bayesian neural networks?"
─────────────────────────────────────────────────────────────────────────────────
event:                  req_9c59
request_received
event:                  index · depth 1

[index → research]
I'll find the foundational reference.

stream_start
event:                  scout · depth 2
stream_start
event: tool_call        /mem
event: tool_call        /mem
event: tool_call        /mem

/search Bayesian neural networks foundational paper

event: tool_call        /search ✓
event: tool_call        /search ✓
event: tool_call        /search ✓

[search returned 5 results]

The most-cited candidate is MacKay 1992. Confirming.

/fetch https://authors.library.caltech.edu/13793/

event: tool_call        /fetch
event: tool_call        /fetch

MacKay 1992, "A Practical Bayesian Framework for Backpropagation
Networks." First application of Laplace approximation to NN
posteriors; ~3,600 citations on Google Scholar.

event: advise           verdict: continue ✓
event: stream_end       stream ended

[research → index]
MacKay 1992. "A Practical Bayesian Framework for Backpropagation
Networks." Foundational; introduces Laplace approximation to NN
posteriors.

event: done             ok=true · 112.1s · $0.0185
```

In the TUI, the `/search` and `/fetch` lines are intercepted, executed by the runtime, and their output fed back into the next turn. Verbose mode shows the raw stream above; normal mode shows the synthesised reply with a spinner for tool activity.

Arbiter is experimental. The writ surface, agent constitutions, and HTTP
shape are subject to change. `/exec` is unsandboxed; treat it accordingly.


## Documentation

- [`docs/getting-started`](https://arbiter.run/docs/getting-started/local) — quickstart paths — quickstart paths
  to first agent reply.
- [`docs/philosophy`](https://arbiter.run/docs/philosophy) — design philosophy: the six
  themes that explain why arbiter is shaped the way it is.
- [`docs/api/`](https://arbiter.run/docs/api) — full HTTP API reference: concept
  pages (tenants, auth, SSE events, fleet streaming, MCP, A2A protocol,
  artifacts, structured memory, operations) and one page per endpoint.
- [`docs/cli/`](https://arbiter.run/docs/cli) — non-interactive command-line
  reference: `--init`, `--send`, `--api`, tenant admin, environment
  variables.
- [`docs/tui/`](https://arbiter.run/docs/tui) — interactive terminal client:
  screen anatomy, slash commands, keybindings, multi-pane layouts,
  streaming and turn lifecycle, session persistence.
- [`CHANGELOG.md`](CHANGELOG.md) — what changed, when. Breaking
  changes are flagged.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — build, tests, PR conventions.
- [`SECURITY.md`](SECURITY.md) — disclosure path for security
  vulnerabilities and operator hardening notes.


## Quick start

Bare minimum for a local install:
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

Linux binary, source builds, OpenAI/Ollama keys, the API server, and one-shot mode are all in [getting-started/local](https://arbiter.run/docs/getting-started/local).

---

Licensed under the [Apache License 2.0](LICENSE).

