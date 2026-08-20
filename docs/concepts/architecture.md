# High-Level Architecture

Arbiter is distributed as a single native binary with three user-facing modes: an interactive TUI, a one-shot CLI, and an HTTP + SSE API.

The following diagram provides a conceptual overview of how these interfaces share the same orchestration runtime, event model, persistent state, agent configuration, and outbound integrations.

This overview intentionally omits implementation details and focuses on the conceptual relationships between Arbiter's major architectural components.

```mermaid
flowchart LR
    USER["User or application"]

    subgraph ARBITER["Arbiter"]
        direction LR

        subgraph FACES["Interfaces"]
            direction TB

            TUI["Interactive TUI"]

            CLI["One-shot CLI<br/><code>arbiter --send</code>"]

            API["HTTP + SSE API<br/><code>arbiter --api</code>"]

            TENANT["API mode<br/>tenant isolation boundary"]
        end

        subgraph RUNTIME["Shared agent runtime"]
            direction TB

            REQUESTS(("Requests"))

            INTENT["Intent engine<br/>classify · route · reconcile"]

            ORCHESTRATION["Shared orchestration loop"]

            subgraph EXECUTION["Agent execution"]
                direction TB

                CONSTITUTION["Constitution<br/>model · role · rules · tool allowlist"]

                AGENT["Agent"]

                WRIT["Writ<br/>prose-embedded runtime actions"]
            end

            ADVISOR{"Optional advisor<br/>consult or gate"}

            EVENTS(("Streaming<br/>events"))

            EVENT_BUS["Event bus<br/>same event model everywhere"]

            REQUESTS --> INTENT
            INTENT --> ORCHESTRATION
            ORCHESTRATION --> AGENT

            CONSTITUTION --> AGENT
            AGENT --> WRIT

            AGENT --> EVENTS
            WRIT --> EVENTS

            AGENT -. optional supervision .-> ADVISOR
            ADVISOR -. continue · redirect · halt .-> ORCHESTRATION
            ADVISOR --> EVENTS
            INTENT --> EVENTS

            EVENTS --> EVENT_BUS
        end

        STORE[("Persistent store<br/><code>~/.arbiter/</code><br/>conversations · request history<br/>structured memory")]

        TUI --> REQUESTS
        CLI --> REQUESTS
        API --> REQUESTS

        EVENT_BUS --> TUI
        EVENT_BUS --> CLI
        EVENT_BUS --> API

        ORCHESTRATION <--> STORE

        TENANT -. scopes API requests .-> API
        TENANT -. scopes persisted state .-> STORE
    end

    subgraph OUTBOUND["Outbound connections"]
        direction LR

        MODELS["Model providers"]

        subgraph CAPABILITIES["Integrations"]
            direction TB

            TOOLS["Tools"]
            MCP["MCP servers"]
            A2A["A2A agents"]
        end
    end

    USER --> TUI
    USER --> CLI
    USER --> API

    AGENT --> MODELS

    WRIT --> TOOLS
    WRIT --> MCP
    WRIT --> A2A
```

## Related concepts

For implementation details and deeper explanations, see:

- [Writ](writ.md)
- [Intent](intent.md)
- [Reconcile](reconcile.md)
- [Advisor](advisor.md)
- [Structured memory](structured-memory.md)
- [MCP](mcp.md)
- [A2A](a2a.md)
- [Voice](voice.md)
- [Tenants](tenants.md)
- [SSE events](sse-events.md)
