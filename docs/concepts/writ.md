# Writ

**Writ** is Arbiter's DSL for agents to issue commands. A writ is a single line that begins with `/` and names a verb the runtime knows how to execute — `/fetch`, `/exec`, `/agent`, `/mem`, `/write`, and so on. The runtime watches the model's token stream, intercepts writs as they appear, runs the corresponding handler, and feeds the result back as a tool-result block on the next turn.

## Why writ-shaped, not tool-use-shaped

Most LLM platforms model agent action as structured tool-use: the model returns a JSON object, the runtime calls the named function, the result comes back on a dedicated turn boundary. Arbiter does not. Agents speak prose; writs are emitted *inside* the prose, line by line, as the agent thinks. The runtime does not negotiate a schema, does not validate arguments against a JSON spec, does not impose a tool-use turn boundary. A writ is whatever line happens to begin with `/` outside a fenced code block.

Several properties fall out of that choice:

- **Prose-embedded.** Agents interleave reasoning, narration, and action in one stream. A `/exec ls` between two sentences is normal.
- **Line-oriented.** The unit of action is a line. The parser is a line-buffered filter on the streaming path, not a grammar.
- **Fence-respecting.** Fenced code blocks (`` ``` `` or `~~~`) are inert — agents can quote shell snippets without accidentally invoking them.
- **Permissive parse, strict dispatch.** A malformed writ never raises a syntax error against the model; it just fails to dispatch. Capability gating happens at dispatch time, not at parse time.

## The verb surface

Writs factor into a small number of orthogonal axes. Each axis has one primary verb and a few variants; uniformity is the point — adding a new axis doesn't reshape the language.

| Axis | Verbs | What's mediated |
|---|---|---|
| Perception | `/fetch`, `/search`, `/browse`, `/read`, `/list` | Pulling state from outside the conversation. |
| Action | `/exec`, `/write` | Pushing state into the world. |
| Delegation | `/agent`, `/parallel`, `/pane` | Other agents as subroutines. |
| Consultation | `/advise` | A higher-capability model as second opinion. See [Advisor](advisor.md). |
| Persistence | `/mem` | Structured memory across turns and conversations. See [Structured memory](structured-memory.md). |
| Federation | `/mcp`, `/a2a` | External tool ecosystems and remote agents. See [MCP](mcp.md) and [A2A](a2a.md). |
| Scheduling | `/schedule` | Defer or recur agent work; runs fire under the API server's tick thread and surface as runs + notifications. See [Scheduler](scheduler.md). |
| Progress tracking | `/todo` | Capture and mark progress on the steps of a request. Tenant-scoped; survives across conversations and pipeline-injects into delegated sub-agents. See [Todos](todos.md). |
| Self-reflection | `/lesson` | Record "this approach failed; try this instead" after a hard-won fix. Agent-scoped; surfaces as a `KNOWN PITFALLS` block at the top of future turns whose prompt matches. See [Lessons](lessons.md). |

Most writs are single-line. Three are blocks with explicit terminators:

- `/write <path>` … `/endwrite` — persist a file.
- `/parallel` … `/endparallel` — fan-out enclosed `/agent` calls concurrently.
- `/mem add entry` … `/endentry` — commit a structured memory entry.

Block terminators are used instead of indentation or braces because models reliably produce closing tokens but don't reliably produce balanced indentation, and terminators survive line-by-line streaming without buffering.

## Agents are first-class

`/agent`, `/parallel`, and `/pane` make other agents callable from inside a writ stream. In a flat verb syntax, this gives the DSL the three primitives of a small concurrency model:

- `/agent <id> <message>` — synchronous call. The calling turn blocks until the sub-agent's reply lands.
- `/parallel` … `/endparallel` — fan-out. Multiple delegations resolved together.
- `/pane <id> <message>` — detached spawn. Fire-and-forget peer.

Composition between agents happens in the same language as composition with tools. There is no separate "multi-agent protocol" sitting above writ.

## Per-agent dialects

Every agent's [Constitution](../agents/create.md) declares a `capabilities` array — a hard allowlist of writs that agent is permitted to dispatch. Two layers govern what an agent actually does:

- **Rules** (soft, prompt-level) tell the model *when* to use a writ.
- **Capabilities** (hard, runtime-level) decide whether a writ the model emits will do anything.

You can't social-engineer a `/exec` out of an agent that doesn't have it in its warrant. A research agent emits the same writ syntax as a backend agent; only the dispatched subset differs.

## Image content in tool results

When a writ retrieves an image, the bytes flow back as image content on the next user-role tool-result message rather than as a textified body. Two writs participate today:

- `/fetch <image-url>` — when the response Content-Type is `image/*`, the body is base64-encoded and attached as an image part. The text envelope reads `[fetched as image #N — <mime>, <bytes>; see image content attached to this turn]` so the model correlates the image to the writ that produced it.
- `/read #<artifact-id>` (or `/read <path>`) — same shape when the artifact's stored mime is `image/*`.

Vision-capable models (Claude, GPT-4o, Gemini 2.x) see the image natively on the next turn. Models without vision support see only the envelope text — agents authored against a vision-incapable model should avoid retrieving images they can't actually consume.

## What writ does not have

- **No expressions, no piping.** You can't write `/fetch | /write`; everything is a string and composition happens across turns or via `/exec`.
- **No structured arguments.** Argument parsing is per-verb and intentionally lax. There is no shared schema.
- **No syntax errors.** A malformed writ is dropped. The model is never penalized at the language level.
- **No global namespace.** Verbs are per-agent via capability allowlists, not a single registry the model has to memorize.

## In one sentence

> *The smallest set of verbs a language model can reliably emit inline, gated by an explicit per-agent allowlist, with first-class delegation and oversight.*

Everything else in arbiter — orchestration, advisor gating, structured memory, MCP and A2A federation — is wired through writ.
