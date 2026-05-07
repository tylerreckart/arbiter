# Hosted preview

A managed arbiter endpoint. Same binary, same writ DSL, same SSE protocol — we run the server, you authenticate with a bearer token. Useful when you want to call arbiter from a web app or automation without operating infrastructure yourself.

> **Status: limited preview.** Access is invite-only while we tune capacity and pricing. Joining the waitlist costs nothing; invites go out in waves.

## What you get

- A bearer token tied to a workspace, scoped to your tenant.
- The full HTTP+SSE API documented in [`api/`](../api/index.md): `POST /v1/orchestrate`, agent CRUD, conversations, structured memory, artifacts, A2A.
- Sandboxed execution. `/exec` is disabled in the hosted environment; `/write` streams files back to your client rather than landing on a server disk. See [`cli/api.md`](../cli/api.md#what-it-doesnt-do).
- Usage telemetry and quota enforcement via the runtime's billing protocol — the same shape as the self-hosted `ARBITER_BILLING_URL` integration, just preconfigured.

## What you don't get (vs. local)

- `/exec` — no shell on the server side. If you want agents that run shell commands, use [local install](local.md).
- A persistent local working directory — agent file output streams as `file` SSE events to your client.
- Direct filesystem access. All persistent state lives behind the API: artifacts, structured memory, scratchpads.

## Request access

<!-- TODO(hosted): replace with the live signup destination once decided.
     Pick one of: email address (e.g. hello@arbiter.dev), waitlist URL,
     or GitHub issue template. The surrounding language is written
     against a "limited preview / waitlist" framing. -->

Join the waitlist: **\<TODO: hosted contact\>**

Tell us: rough use case (research agent, code review bot, embedded assistant, …), expected request volume, and whether you have provider keys you'd like the workspace to use directly. We reply within a few business days during preview.

## Once provisioned

You'll receive a bearer token and an endpoint URL. Smoke-test it:

```bash
curl -N -H "Authorization: Bearer atr_..." \
     -H "Content-Type: application/json" \
     -d '{"agent":"index","message":"hello"}' \
     https://<your-endpoint>/v1/orchestrate
```

The response is a Server-Sent Events stream. Event shapes are documented in [`concepts/sse-events.md`](../concepts/sse-events.md); the full request/response contract for the orchestrate call is at [`api/orchestrate.md`](../api/orchestrate.md).

From there, the rest of the API works the same as a local `--api` server — the hosted endpoint is just a `--api` server somewhere else.

## Next steps

- **Author your own agent.** Schema reference: [`api/agents/create.md`](../api/agents/create.md). Concepts: [Writ](../concepts/writ.md), [Advisor](../concepts/advisor.md).
- **Read the design philosophy.** [`philosophy.md`](../philosophy.md).
- **Wire up integrations.** [MCP servers](../concepts/mcp.md) for external tools, [A2A](../concepts/a2a.md) for remote agents.
