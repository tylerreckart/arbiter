# Concepts

The ideas that shape Arbiter: how agents speak to the runtime, how work is supervised, how memory persists, and how events become runs.

Start here if you want the model before the reference material.

| Concept | What it covers |
|---------|----------------|
| [Writ](writ.md) | The slash-command DSL agents emit inline |
| [Advisor](advisor.md) | Structural supervision gates (`CONTINUE` / `REDIRECT` / `HALT`) |
| [SSE events](sse-events.md) | The stream contract shared by TUI and HTTP |
| [Structured memory](structured-memory.md) | Typed, temporal facts with layered retrieval |
| [Tenants](tenants.md) | Isolation boundaries in API mode |
| [Authentication](authentication.md) | Bearer tokens and tenant scope |
| [Scheduler](scheduler.md) | Natural-language schedules and run history |
| [Sandbox](sandbox.md) | Constrained execution for risky tools |
| [MCP](mcp.md) | Stdio and remote tool servers |
| [A2A](a2a.md) | Agent-to-agent federation |
| [Artifacts](artifacts.md) | Persisted files tied to conversations |
| [Todos](todos.md) | Progress checklists agents and clients share |
| [Lessons](lessons.md) | Durable learnings extracted from runs |
| [Search](search.md) | External search fallbacks for `/search` |
| [Data model](data-model.md) | Core objects and field schemas |
| [Durable execution](durable-execution.md) | In-flight requests you can reconnect to |
| [Fleet streaming](fleet-streaming.md) | Multiplexed streams across agents |
| [Operations](operations.md) | Deploy notes, proxies, and observability |
