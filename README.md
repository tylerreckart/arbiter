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

## Why arbiter

- **Writ agentic DSL.** Agents emit slash commands inline
  with their prose. No function-calling schema, no tool-use turn
  boundary. The runtime line-buffers the stream and dispatches as the
  model writes. See [writ](docs/concepts/writ.md).
- **Multi-agent composition as a language primitive.** `/agent` calls a
  sub-agent synchronously, `/parallel` fans out, `/pane` spawns
  detached. Agents are first-class verbs in the same DSL as tools.
- **Structural advisor gating.** A higher-capability model can supervise
  the executor's terminating turn at the runtime level, signalling
  `CONTINUE` / `REDIRECT` / `HALT`. Execution and judgment are separate
  loops. See [advisor](docs/concepts/advisor.md).
- **Single binary, local-first.** <2 MB C++ runtime, no Python or Node
  dependency. SQLite-backed local state under `~/.arbiter/`. The HTTP
  server is opt-in, not the default.

## Example session

What an agent's reply actually looks like — writs interleaved with the prose, the runtime intercepting them as they're emitted:

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

Linux binary, source builds, OpenAI/Ollama keys, the API server, and one-shot mode are all in [getting-started/local](docs/getting-started/local.md). For a managed endpoint instead of installing locally, see [getting-started/hosted](docs/getting-started/hosted.md).


## Memory benchmarks

Arbiter ships a structured-memory layer — typed entries, FTS retrieval, optional LLM rerank — that agents query through `/mem` and that operators can drive directly via `/v1/memory/entries`. Retrieval quality on [LongMemEval](https://github.com/xiaowu0162/LongMemEval) (500 questions, ~247K conversational turns), where **R@K** is the fraction of
questions with at least one ground-truth turn in the top K
results:

| Variant      | R@1   | R@5   | R@10  | p50      | p95     |
|--------------|------:|------:|------:|---------:|--------:|
| `bm25`       | 14.2% | 35.2% | 42.0% |   149 ms |  370 ms |
| `graduated`  | 49.4% | 81.4% | 88.6% |    57 ms |  109 ms |
| `rerank`     | 72.6% | 90.4% | 91.6% |  1637 ms | 2157 ms |

What each variant actually measures:

- **`bm25`** — FTS5 + Okapi-BM25 across the entire tenant corpus, no
  scope hint. Query-side stopword stripping plus a phrase-boost
  clause (`"tok1 tok2 ..." OR tok1 OR tok2 ...`) concentrate scoring
  on content tokens and reward proximity matches.
- **`graduated`** — conversation-scoped first pass with tenant-wide
  fallback if the first pass returns fewer than `limit` candidates.
- **`rerank`** — `graduated` retrieval drawn from a 25-candidate pool,
  then reordered by an LLM (here `claude-haiku-4-5`) over the full
  list before trimming to the requested `limit`. Each candidate is
  shown to the reranker with up to 800 bytes of content excerpt so
  the answer-bearing text is visible end-to-end. One extra LLM call
  per query.

---

Licensed under the [Apache License 2.0](LICENSE).

