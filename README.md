# Arbiter

A multi-agent orchestration runtime for the terminal and the network.

Arbiter runs a master agent named `index` that decides whether to answer a
request directly or delegate it to a specialist, streams the work back
live, and persists the durable output — files, structured memory, agent
definitions — across sessions. Agents drive the runtime with a small set
of slash commands they emit inline in their replies: fetch a URL, run a
shell command, write a file, record a memory, call each other.

It runs in three shapes.

- `arbiter` — an interactive terminal client. A persistent header shows
  the current agent and model. Commands you type while agents are
  working queue up and dispatch in order. Parallel sub-agents render in
  their own panes.
- `arbiter --api` — a multi-tenant HTTP server with bearer-token auth and
  streaming responses. Each tenant gets isolated agents, memory, and
  artifacts.
- `arbiter --send <agent> <message>` — a one-shot dispatch for scripts and
  cron jobs.

Arbiter is experimental. The slash-command surface, agent constitutions,
and HTTP shape are subject to change. `/exec` is unsandboxed; treat it
accordingly.


## Documentation

- [`docs/api/`](docs/api/index.md) — full HTTP API reference: concept
  pages (tenants, auth, SSE events, fleet streaming, MCP, artifacts,
  structured memory, operations) and one page per endpoint.
- [`docs/cli`](docs/cli/index.md) - TODO
- [`docs/tui`](docs/tui/index.md) - TODO
- [`PHILOSOPHY.md`](docs/philosophy.md) - TODO
- [`CHANGELOG.md`](CHANGELOG.md) — what changed, when. Breaking
  changes are flagged.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — build, tests, PR conventions.
- [`SECURITY.md`](SECURITY.md) — disclosure path for security
  vulnerabilities and operator hardening notes.


## Install

Build from source:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    sudo cmake --install build

Requires OpenSSL, libcurl, SQLite3, and a C++20 compiler. libedit or GNU
readline is optional but recommended for the terminal client.


## Setup

Set whichever provider keys you plan to use — only one is required.

    export ANTHROPIC_API_KEY="sk-ant-..."
    export OPENAI_API_KEY="sk-..."

Keys can also live at `~/.arbiter/api_key` (Anthropic) or
`~/.arbiter/openai_api_key` (OpenAI).

Then:

    arbiter --init      # generate auth token, create starter agents
    arbiter             # launch the terminal client


## Running

### Terminal client

    arbiter

Type a message to send it to the current agent. Switch agents with
`/use <name>`. Send to a specific agent with `/send <name> <message>`.
Common control commands: `/agents`, `/status`, `/tokens`, `/reset`.

### API server

    arbiter --add-tenant acme                   # prints the tenant's bearer token
    arbiter --api --port 8080 --bind 0.0.0.0

Authenticate with `Authorization: Bearer <token>`.

### One-shot

    arbiter --send reviewer "review: if (arr.length = 0) return;"


## Benchmarks

Retrieval quality on [LongMemEval](https://github.com/xiaowu0162/LongMemEval). 500 questions, ~247K conversational turns. **R@K** is the fraction of
questions where at least one ground-truth turn appears in the top K
results from `GET /v1/memory/entries`.

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

