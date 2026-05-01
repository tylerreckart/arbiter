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

Authenticate with `Authorization: Bearer <token>`. Set
`ARBITER_BILLING_URL` to delegate eligibility checks and usage
tracking to an external billing service; unset, the runtime acts
as a thin pass-through using the configured provider keys, with
no caps. Anyone running arbiter commercially needs to provide
their own billing service that implements the runtime's billing
protocol (three HTTP endpoints under `/v1/runtime/`; the in-tree
client is a reference for what the runtime sends and expects to
receive). Per-endpoint documentation lives in
[`docs/api/`](docs/api/index.md).

### One-shot

    arbiter --send reviewer "review: if (arr.length = 0) return;"


## Benchmarks

Retrieval quality on [LongMemEval](https://github.com/xiaowu0162/LongMemEval). 500 questions, ~247K conversational turns. **R@K** is the fraction of
questions where at least one ground-truth turn appears in the top K
results from `GET /v1/memory/entries`.

| Variant      | R@1   | R@5   | R@10  | p50      | p95     |
|--------------|------:|------:|------:|---------:|--------:|
| `bm25`       | 16.6% | 34.8% | 42.2% |   513 ms |  957 ms |
| `graduated`  | 49.8% | 80.6% | 88.0% |   129 ms |  187 ms |
| `rerank`     | 56.8% | 85.2% | 88.0% |  1022 ms | 1949 ms |

What each variant actually measures:

- **`bm25`** — FTS5 + Okapi-BM25 across the entire tenant corpus, no
  scope hint.
- **`graduated`** — conversation-scoped first pass with tenant-wide
  fallback if the first pass returns fewer than `limit` candidates.
- **`rerank`** — `graduated` retrieval, then the top-N candidates
  reordered by an LLM (here `claude-haiku-4-5`). One extra LLM call
  per query.

## What agents can do

Agents emit slash commands as part of their replies. The orchestrator
parses them, runs them, and feeds results back as a tool-result block
before the next turn.

**Web research**

- `/search <query> [top=N]` — web search; ranked URLs.
- `/fetch <url>` — static HTTP fetch; HTML stripped to readable text.
- `/browse <url>` — JS-rendering fetch via Playwright. Use when `/fetch`
  hits Cloudflare, paywalls, or pages that only render content from
  JavaScript.

**Files**

- `/write <path>` — write a file. Content follows until `/endwrite`.
  Default is ephemeral: the user sees it inline but the server doesn't
  keep it.
- `/write --persist <path>` — same, but also save the file to the
  conversation's artifact store. Durable; readable later.
- `/read <path>` or `/read #<id>` — read a saved artifact in this
  conversation.
- `/read #<id> via=mem:<entry_id>` — read an artifact across
  conversations using a memory entry as the access capability.
- `/list` — list saved artifacts in this conversation.

**Shell**

- `/exec <command>` — run a shell command; stdout and stderr are
  returned. Not sandboxed.

**Orchestration**

- `/agent <id> <message>` — call a sub-agent synchronously; its reply
  folds into the current turn.
- `/parallel ... /endparallel` — fan out N `/agent` calls concurrently.
- `/pane <id> <message>` — call a sub-agent asynchronously; its result
  arrives in a later turn as a fresh `[PANE RESULT]` message.

**Memory — _free-form scratchpad_**

- `/mem write <text>` — append a note.
- `/mem read` — load the scratchpad into context.
- `/mem show`, `/mem clear` — display, delete.
- `/mem shared write|read|clear` — pipeline-shared scratchpad visible to
  every agent in the conversation.

**Memory — _structured graph_**

- `/mem entries [type=...] [tag=...]` — list curated graph nodes.
- `/mem entry <id>` — fetch one entry plus its edges.
- `/mem search <query>` — relevance-ranked search across title, tags,
  content, and source.
- `/mem expand <id> [depth=N]` — fetch the surrounding subgraph.
- `/mem density <id>` — count edges and reach; probe before redundant
  research.
- `/mem add entry <type> <title>` followed by a body and `/endmem` —
  add a typed node. The body is required and is the text future searches
  rank against. Types: user, feedback, project, reference, learning,
  context.
- `/mem add link <src_id> <relation> <dst_id>` — add a directed edge.
  Relations: relates_to, refines, contradicts, supersedes, supports.

**MCP**

- `/mcp tools` — list tools exposed by the configured MCP servers.
- `/mcp call <server>.<tool> <json-args>` — invoke a tool.

**Advisor**

- `/advise <question>` — one-shot consult against a more capable model.
  Available only when the agent's constitution sets `advisor_model`.

**Discovery**

- `/help` — list help topics.
- `/help <topic>` — detailed reference for one slash command. Topics
  include web, write, exec, delegation, mem, artifacts, mcp, advise.


## Agent definitions

`arbiter --init` creates a set of starter agents. Each agent is a JSON
document.

    {
      "name": "reviewer",
      "role": "code-reviewer",
      "personality": "Senior engineer. Finds fault efficiently.",
      "brevity": "ultra",
      "max_tokens": 512,
      "temperature": 0.2,
      "model": "claude-sonnet-4-6",
      "goal": "Inspect code. Identify defects. Prescribe remedies.",
      "rules": [
        "Defects first, style second.",
        "Prescribe the concrete fix, never vague counsel."
      ],
      "capabilities": ["/exec", "/write", "/agent"]
    }

`capabilities` lists the slash commands the agent is expected to use.
The system prompt only documents those commands, keeping the per-turn
token cost proportional to what the agent actually does. Leave it empty
on the master orchestrator to enable everything.

### Brevity

- `lite` — full grammar, no filler.
- `full` — drop articles, fragments permitted. Default.
- `ultra` — maximum compression. Abbreviations, arrows, minimal words.

### Modes

- _(unset)_ — standard `index` voice.
- `writer` — full prose mode for long-form output. Complete sentences,
  format guidance, no compression.
- `planner` — decomposition mode. Produces structured plan files with
  phases, dependencies, and acceptance criteria.

The bundle composer applies only to the unset mode; `writer` and `planner`
have their own dedicated base prompts.

## Model providers

Each agent's `model` field is routed by prefix.

- `claude-*` (or a bare model id) — Anthropic. Endpoint:
  `api.anthropic.com`. Key from `ANTHROPIC_API_KEY` or
  `~/.arbiter/api_key`.
- `openai/<model>` — OpenAI. Endpoint: `api.openai.com`. Key from
  `OPENAI_API_KEY` or `~/.arbiter/openai_api_key`.
- `ollama/<model>` — Ollama (OpenAI-compatible). Endpoint from
  `$OLLAMA_HOST`, default `http://localhost:11434`. No key required.

Cross-provider mixes are first-class: a local Ollama executor paired
with a cloud advisor model is a common cost-efficient pattern. Bulk
work runs locally; only `/advise` calls hit the cloud.

OpenAI reasoning models (o-series) automatically use
`max_completion_tokens` and omit `temperature`, which they reject.

Smaller local models (≤14B parameters) follow the `/exec` / `/write`
text conventions less reliably than cloud providers. Expect occasional
missed tool calls and looser adherence to brevity rules.


## Advisor model

The advisor pattern lets a cheaper executor model call out to a smarter
one for hard judgment calls — architectural tradeoffs, ambiguity
resolution, multi-step planning. Set `advisor_model` in the executor's
constitution:

    {
      "model": "claude-haiku-4-5-20251001",
      "advisor_model": "claude-opus-4-7"
    }

The executor gains a new command, `/advise <question>`, which it can
emit inline like any other slash command. The advisor sees only the
text after `/advise`; nothing from the prior conversation leaks in. This
forces the executor to pose self-contained questions and keeps advisor
calls cheap and predictable. Consults are capped at two per turn — a
third desired call means the task is under-scoped, and the executor is
told to deliver what it has.

Routing is by model-string prefix, so executor and advisor can be on
different providers. Advisor tokens post to the caller's cost ledger
using the advisor's model pricing.


---

Licensed under the [Apache License 2.0](LICENSE).

