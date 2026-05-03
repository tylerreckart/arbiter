# Arbiter CLI

Arbiter is a single binary with three operating modes — interactive, one-shot, and server — plus a handful of admin commands. Which mode you pick depends on whether you want a terminal session, a scripted invocation, or a long-running HTTP service.

| You want…                                            | Run                                          |
|------------------------------------------------------|----------------------------------------------|
| Interactive multi-pane TUI                           | `arbiter`                                    |
| One agent reply, piped output, no TTY                | `arbiter --send <agent> <message>`           |
| HTTP+SSE orchestration server for other clients      | `arbiter --api [--port N] [--bind ADDR]`     |
| Initialize `~/.arbiter/` with example agents         | `arbiter --init`                             |
| Manage `--api` tenant identities                     | `arbiter --add-tenant`, `--list-tenants`, `--disable-tenant`, `--enable-tenant` |

The interactive mode is documented separately under [`docs/tui`](../tui/index.md). Everything else is here.

## Full flag reference

```
arbiter                                    Interactive REPL (the TUI)
arbiter --init                             Create ~/.arbiter/ + example agents
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

- [`--init`](init.md) — what gets created, the `~/.arbiter/` layout.
- [`--send`](send.md) — the one-shot mode for scripts, pipes, and CI hooks.
- [`--api`](api.md) — the HTTP server mode: flags, behaviour, links to the endpoint docs.
- [Tenant admin](tenants.md) — managing `--api` bearer tokens.
- [Environment](environment.md) — every env var arbiter reads, what overrides what.

## Choosing a mode

**Interactive.** The default. Multi-pane TUI, persistent session per cwd, all slash commands available. Built for humans typing into a terminal. Reach for it when exploring, iterating with an agent, or running long sessions.

**One-shot (`--send`).** No TTY, no panes, no streaming. Sends one message to one agent, prints the reply on stdout, exits non-zero on error. Built for scripts and pipelines:

```bash
echo "$DIFF" | arbiter --send reviewer "$(cat)"
arbiter --send research "summarise the top 3 issues" > /tmp/summary.md
```

The agent runs the full orchestration loop internally (tool calls, sub-agent delegation) — you just don't see the intermediate steps; you see the final synthesised reply.

**API (`--api`).** A long-running HTTP+SSE server. Real applications (web UIs, automations, mobile clients) call it instead of forking the binary per request. Multi-tenant: every request authenticates with a bearer token from `--add-tenant`, all conversations / artifacts / memory / scratchpads are tenant-scoped. See [`docs/api`](../api/index.md) for the endpoint catalogue.

The three modes share the same on-disk state — agents under `~/.arbiter/agents/*.json`, API keys read from `~/.arbiter/api_key` (or env), provider keys at the same precedence everywhere. A change made in one mode is visible to the others on next launch.

## Exit codes

| Code  | Meaning                                                              |
|-------|----------------------------------------------------------------------|
| `0`   | Success.                                                             |
| `1`   | Generic failure: unknown flag, missing argument, unhandled exception, agent error in `--send`. The error message lands on stderr. |
| Other | Forwarded from the runtime: `--api` exits via signal handlers (`SIGINT`/`SIGTERM`) and reports the signal in its exit status as usual. |
