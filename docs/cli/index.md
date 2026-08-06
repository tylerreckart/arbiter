# Arbiter CLI

Arbiter is a single binary with three operating modes — interactive, one-shot, and server — plus a handful of admin commands. Which mode you pick depends on whether you want a terminal session, a scripted invocation, or a long-running HTTP service.

| You want…                                            | Run                                          |
|------------------------------------------------------|----------------------------------------------|
| Interactive multi-pane TUI                           | `arbiter`                                    |
| Thin-client TUI against a remote `--api` host        | `arbiter --connect [URL] [--token TOKEN]`    |
| One agent reply, piped output, no TTY                | `arbiter --send <agent> <message>`           |
| HTTP+SSE orchestration server for other clients      | `arbiter --api [--port N] [--bind ADDR]`     |
| Initialize `~/.arbiter/` with example agents         | `arbiter --init`                             |
| Configure `/search`, `/browse`, and MCP servers      | `arbiter --setup-tools`                      |
| Manage `--api` tenant identities                     | `arbiter --add-tenant`, `--list-tenants`, `--disable-tenant`, `--enable-tenant` |

The interactive mode is documented separately under [`docs/tui`](../tui/index.md). Everything else is here.

## Full flag reference

```
arbiter                                    Interactive REPL (the TUI)
arbiter --connect [URL] [--token TOKEN]    Thin-client TUI → remote --api
arbiter --init                             Create ~/.arbiter/ + example agents
arbiter --setup-tools                      Interactive wizard for search / browse / MCP
arbiter --send <agent> <message>           One-shot — print agent reply to stdout
arbiter --api [--port N] [--bind ADDR]     HTTP+SSE orchestration server
              [--verbose]
arbiter --add-tenant <name>                Provision a tenant + API key
arbiter --list-tenants                     Print active and disabled tenants
arbiter --disable-tenant <id|name>         Revoke a tenant's access
arbiter --enable-tenant <id|name>          Restore access
arbiter --help                             Print built-in help
```

## Pages

- [Getting started](../getting-started/index.md) — pick a path:
  - [Local install](../getting-started/local.md) — install + first run on your own machine.
- [`--init`](init.md) — what gets created, the `~/.arbiter/` layout.
- [`--setup-tools`](setup-tools.md) — interactive MCP / search / browse wizard.
- [`--send`](send.md) — the one-shot mode for scripts, pipes, and CI hooks.
- [`--api`](api.md) — the HTTP server mode: flags, behaviour, links to the endpoint docs.
- [`--connect`](connect.md) — thin-client TUI against a remote `--api` instance.
- [Tenant admin](tenants.md) — managing `--api` bearer tokens.
- [A2A agents](a2a-agents.md) — outbound remote-agent registry + the `/a2a` slash command.
- [Environment](environment.md) — every env var arbiter reads, what overrides what.

## Choosing a mode

**Interactive.** The default. Multi-pane TUI, global conversation store under `~/.arbiter/conversations/`, all slash commands available. Built for humans typing into a terminal. Reach for it when exploring, iterating with an agent, or running long sessions.

**One-shot (`--send`).** No TTY, no panes, no streaming. Sends one message to one agent, prints the reply on stdout, exits non-zero on error. Built for scripts and pipelines:

```bash
echo "$DIFF" | arbiter --send reviewer "$(cat)"
arbiter --send research "summarise the top 3 issues" > /tmp/summary.md
```

The agent runs the full orchestration loop internally (tool calls, sub-agent delegation) — you just don't see the intermediate steps; you see the final synthesised reply.

**API (`--api`).** A long-running HTTP+SSE server. Real applications (web UIs, automations, mobile clients) call it instead of forking the binary per request. Multi-tenant: every request authenticates with a bearer token from `--add-tenant`, all conversations / artifacts / memory / scratchpads are tenant-scoped. See [`docs/api`](../api/index.md) for the endpoint catalogue.

**Remote TUI (`--connect`).** Same binary, client mode: render the TUI locally while turns stream from a remote `--api` over HTTP+SSE. No local provider keys. See [`connect.md`](connect.md).

The interactive and API modes share the same on-disk state when they run on the same host — agents under `~/.arbiter/agents/*.json`, the OpenRouter key from `OPENROUTER_API_KEY` or `~/.arbiter/openrouter_api_key`, and provider settings at the same precedence everywhere. A `--connect` client does not use the local provider key store; keys are required only on the API host.

## Exit codes

| Code  | Meaning                                                              |
|-------|----------------------------------------------------------------------|
| `0`   | Success.                                                             |
| `1`   | Generic failure: unknown flag, missing argument, unhandled exception, agent error in `--send`. The error message lands on stderr. |
| Other | Forwarded from the runtime: `--api` exits via signal handlers (`SIGINT`/`SIGTERM`) and reports the signal in its exit status as usual. |
