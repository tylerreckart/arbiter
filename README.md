# Arbiter

A multi-agent orchestration runtime for the terminal and the network. Arbiter
runs a master agent (`index`) that delegates to a fleet of typed sub-agents,
streams their work back over SSE or to a TUI, and persists the durable
output — files, structured memory, agent definitions — in a per-tenant SQLite
ledger.

It runs in three shapes:

- **TUI** (`arbiter`) — interactive single-tenant terminal client, a persistent
  command queue, and live panes for parallel sub-agents.
- **HTTP/SSE API** (`arbiter --api`) — multi-tenant server with bearer-token
  auth, per-tenant data isolation, billing meter, and streaming responses.
- **One-shot** (`arbiter --send <agent> <msg>`) — single request, single reply.

> Arbiter is experimental. The agent constitutions, slash-command surface,
> and HTTP shape are subject to change. `/exec` is unsandboxed; treat it
> accordingly.

---

## What the runtime does

### Multi-agent orchestration

The master agent reads each request and decides whether to answer directly,
delegate to one specialist (`/agent`), fan out across many in parallel
(`/parallel`), or spawn an asynchronous pane (`/pane`). Sub-agents run on
their own threads, with their own histories, against any wired model
provider. Each `/agent` invocation receives a fresh ephemeral copy of the
named agent — siblings in `/parallel` don't share state.

When a turn dispatches sub-agents, the master is gated against synthesising
a reply until the children return. The user sees a routing decision, then
the children's output, then the master's synthesis — never premature
conclusions in the dispatch turn.

### The slash DSL

Agents drive the runtime by emitting slash commands inline in their
responses. The orchestrator parses, executes, and feeds results back as
`[TOOL RESULTS]` blocks before the next turn.

| Command | Purpose |
|---|---|
| `/search <query> [top=N]` | Web search (Brave); ranked URLs |
| `/fetch <url>` | Static HTTP fetch via libcurl; HTML stripped to text |
| `/browse <url>` | JS-rendering fetch via Playwright MCP — Cloudflare, paywalls, SPAs |
| `/exec <shell>` | Run shell; stdout+stderr returned (not sandboxed) |
| `/agent <id> <msg>` | Synchronous sub-agent invocation |
| `/parallel ... /endparallel` | Concurrent fan-out across N `/agent` calls |
| `/pane <id> <msg>` | Asynchronous sub-agent in its own pane |
| `/write <path>` | Ephemeral file write (streamed to client) |
| `/write --persist <path>` | Same, plus saved to the conversation's artifact store |
| `/read <path>` &#124; `#<aid>` | Read a persisted artifact in this conversation |
| `/read #<aid> via=mem:<id>` | Read across conversations using a memory entry as capability |
| `/list` | List persisted artifacts |
| `/mem write` &#124; `read` &#124; `show` &#124; `clear` | Per-agent free-form scratchpad |
| `/mem shared write` &#124; `read` &#124; `clear` | Pipeline-shared scratchpad (visible to all agents) |
| `/mem entries [type=...] [tag=...]` | List structured-graph nodes |
| `/mem entry <id>` | Fetch one entry plus its edges |
| `/mem search <query>` | Ranked search across title / tags / content / source |
| `/mem expand <id> [depth=N]` | Pull surrounding subgraph (depth ≤ 2, ≤ 50 nodes) |
| `/mem density <id>` | In/out degree + 2-hop reach — probe before redundant research |
| `/mem add entry <type> <title>` | Block form with required body; types: user, feedback, project, reference, learning, context |
| `/mem add link <src> <rel> <dst>` | Directed edge; rels: relates_to, refines, contradicts, supersedes, supports |
| `/mcp tools` &#124; `/mcp call <server>.<tool>` | Invoke MCP server tools |
| `/advise <question>` | One-shot consult against a smarter advisor model |
| `/help [<topic>]` | Detailed reference loaded on demand |

`/help` is the discovery mechanism: the system prompt carries a compressed
inventory plus turn-by-turn rules; verbose how-to-use prose lives in the
help corpus and is loaded only when an agent asks for it. This keeps the
per-turn token cost proportional to what the agent actually uses.

### Structured memory graph

Beyond the free-form scratchpad, Arbiter persists a typed, directed graph
of memory entries the agent contributes during normal work. Each entry has
a type, a title, a required content body, optional tags, and an optional
artifact link. Edges between entries carry a relation kind.

- **Types** partition the graph: `user`, `feedback`, `project`, `reference`,
  `learning`, `context`.
- **Relations** encode reasoning: `relates_to`, `refines`, `contradicts`,
  `supersedes`, `supports`.
- **Bodies are required** — entries without synthesised retrievable text
  are rejected. The body is what `/mem search` ranks against.
- **Cascade-delete** on relations: dropping an entry takes its edges
  with it, never leaving dangling references.

Reads (`entries`, `entry`, `search`, `expand`, `density`) are sophisticated:
ranked relevance, neighbour titles inlined, depth-limited subgraph fetch,
graph-density probe to detect redundant work.

Writes are direct — agents and HTTP callers both write into the curated
graph and reads surface every row. There is no review queue.

### Artifact store

Every `/write --persist` lands a row in the per-(tenant, conversation)
artifact store: a sanitized path, a SHA-256 ETag, three-tier byte quotas,
and a `/raw` endpoint for clients. Artifacts can be linked to memory
entries via a nullable `artifact_id` foreign key; deleting the entry
cascades to the artifact link without orphaning the bytes.

The cross-conversation read pattern uses a memory entry as a capability:
`/read #<aid> via=mem:<entry_id>` is the only way to retrieve an artifact
from outside the conversation that wrote it. This makes the graph the
discovery layer for long-lived files — `/mem search` finds the entry,
`/mem entry <id>` prints the exact `/read` line.

### Capability bundles

Each agent declares its `capabilities` — the slash-commands it's expected
to use. The constitution composer emits only the inventory and rules for
the bundles those capabilities map to:

| Bundle | Slash commands taught |
|---|---|
| `web` | `/search`, `/fetch`, `/browse` + escalation pattern |
| `exec` | `/exec` |
| `write` | `/write`, `/write --persist` + path safety |
| `read` | `/read`, `/list` + `via=mem` capability rule |
| `mem` | `/mem` (all subforms) + structured-graph proactivity rule |
| `delegation` | `/agent`, `/parallel`, `/pane` + delegation-turn discipline |
| `mcp` | `/mcp tools`, `/mcp call` (opt-in only — never default) |

Empty `capabilities` resolves to all default bundles (master orchestrator
behaviour). A devops agent declaring just `["/exec", "/write"]` carries
roughly 40% of the master's prompt; a research agent's bundle profile is
about 85% of master. The slash DSL itself is the same — bundles only
control what's documented in the system prompt.

### Multi-provider routing

Each agent's `model` field is routed by prefix:

| Prefix | Provider | Endpoint | Key |
|---|---|---|---|
| `claude-*` (or any bare id) | Anthropic | `api.anthropic.com` | `ANTHROPIC_API_KEY` |
| `openai/<model>` | OpenAI | `api.openai.com` | `OPENAI_API_KEY` |
| `ollama/<model>` | Ollama (OpenAI-compat) | `$OLLAMA_HOST` | none |

Anthropic prompt-caching is wired on both the system block and the last
message of every turn. OpenAI's implicit caching is reflected in the cost
ledger when the API reports `cached_tokens`. Ollama turns don't benefit
from cache discounts.

Cross-provider mixes are first-class: a local Ollama executor paired with
a `claude-opus-4-7` advisor is the canonical cost-efficient pattern.

### Advisor pattern

Set `advisor_model` on a constitution to give the agent `/advise` — a
one-shot consult against a more capable model. The advisor sees only the
text after `/advise`; nothing from the conversation history leaks in. The
executor must pose a self-contained question.

The constitution caps consults at two per turn — a third desired call
means the task is under-scoped and the executor is told to deliver what
it has. Advisor tokens post to the *caller's* cost ledger using the
advisor's model pricing, so `/tokens` accurately attributes spend even
across providers.

### MCP integration

Arbiter speaks MCP (Model Context Protocol) as a client. Servers are
declared per-request and spawned as stdio subprocesses managed by a
per-request `mcp::Manager`; lifetime ends with the request. The bundled
Playwright server backs `/browse` for JS-heavy sites that defeat
`/fetch`. Other MCP servers are reachable via `/mcp tools` and
`/mcp call <server>.<tool> <json-args>`.

### Streaming + delegation

Agent turns are streamed token-by-token to the client over SSE. Every
sub-agent depth and every parallel child opens its own stream, so the
client can render multiple panes at once. The orchestrator gates the
master's text output during dispatch turns: while sub-agents are still
running, the master's prose is suppressed and a `→ delegating: ...`
status line appears in its place. Only after the children return does
the master's synthesis stream through.

### HTTP API surface

`arbiter --api --port 8080` serves:

- `POST /v1/orchestrate` — main dispatch (SSE streaming)
- `GET /v1/health` — liveness
- `GET /v1/models` — provider/model inventory
- `POST /v1/requests/cancel` — cancel an in-flight stream
- `GET|POST|PATCH|DELETE /v1/agents` — per-tenant agent catalogue
- `GET|POST|DELETE /v1/conversations/.../artifacts` — artifact CRUD + `/raw`
- `GET|POST|PATCH|DELETE /v1/memory/entries` — structured-graph entries
- `POST|GET|DELETE /v1/memory/relations` — directed edges
- `GET /v1/memory/graph` — full graph for visualisation
- `GET /v1/memory` — list this tenant's scratchpads
- `GET /v1/memory/:agent_id` — read one agent's scratchpad (or `/v1/memory/shared`)
- `/v1/admin/*` — tenant + billing administration

All endpoints are tenant-scoped via bearer token. Per-tenant SQLite uses
`SQLITE_OPEN_FULLMUTEX` for concurrent request safety. Detailed
per-endpoint documentation is in `docs/api/`.

---

## Install

### Homebrew (macOS)

```bash
brew tap arbitercorp/tap
brew install arbiter
```

### Build from source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Requires: OpenSSL, libcurl, SQLite3, a C++20 compiler. libedit / GNU
readline is optional but recommended for the TUI.

### Setup

```bash
export ANTHROPIC_API_KEY="sk-ant-..."
export OPENAI_API_KEY="sk-..."          # optional
arbiter --init                           # generates token, creates starter agents
arbiter                                  # launch TUI
```

Keys can also live at `~/.arbiter/api_key` or `~/.arbiter/openai_api_key`.

---

## Running

### TUI

```bash
arbiter
```

A persistent header shows current agent, model, and cost. Commands you
type while agents are working queue up and dispatch in order. Parallel
sub-agents render in their own panes.

### Multi-tenant API server

```bash
arbiter --add-tenant acme --cap 100         # USD cap; 0 = unlimited
arbiter --gen-token acme                    # bearer token for the tenant
arbiter --api --port 8080 --bind 0.0.0.0    # serve
```

Authenticate with `Authorization: Bearer <token>`. Each request runs
under a fresh `Orchestrator` instance scoped to the tenant — concurrent
calls don't share agent state.

Verbose mode (`--verbose`, or `ARBITER_API_VERBOSE=1`) mirrors the SSE
stream to stderr in a colourised, demo-friendly format: agent names get
per-agent palette colours, tool calls show `<tool> ✓` / `✗`, file writes
show `📄 <path> (<size>)`, and the final DONE line carries the
request-wide token + cost tally.

### One-shot

```bash
arbiter --send reviewer "review: if (arr.length = 0) return;"
```

---

## Agent definitions

`arbiter --init` seeds starter agents in `~/.arbiter/agents/`:

| Agent | Role | Capabilities |
|---|---|---|
| `index` | Master orchestrator (built-in) | All bundles |
| `research` | Web research analyst | web, mem, delegation |
| `reviewer` | Code reviewer | exec, write, delegation |
| `writer` | Long-form content (writer mode) | web, write, exec, delegation, mem |
| `devops` | Shell, git, Docker, infra | exec, fetch, delegation, write |
| `planner` | Task decomposition (planner mode) | exec, fetch, delegation, write |
| `marketer`, `social` | Marketing / social copy | write, fetch, delegation |

Each agent is a JSON document. Persisted on disk for the TUI; persisted
in the per-tenant SQLite catalogue when running under `--api`.

```json
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
```

### Brevity

| Level | Style |
|---|---|
| `lite` | Full grammar, no filler |
| `full` | Drop articles, fragments permitted (default) |
| `ultra` | Maximum compression — abbreviations, arrows, minimal words |

### Modes

| Mode | Effect |
|---|---|
| _(unset)_ | Standard `index` voice — compressed, declarative, capability-bundle prompt |
| `writer` | Full-prose mode — complete sentences, format guidance, no compression |
| `planner` | Decomposition mode — structured plan output, always writes to file |

The bundle composer applies only to the unset mode; `writer` and `planner`
have their own dedicated base prompts.
