# Getting started

Arbiter is an open-source, local-first multi-agent workspace for the terminal.
Run parallel conversations, inspect tool activity and diffs as work streams,
and return to saved threads without leaving your shell.

Start with the [local install](local.md) and the interactive TUI. The same
runtime also supports one-shot CLI calls and an HTTP+SSE server when you want
to automate it. To expose `--api` on a VPS with TLS, follow the
[secure remote API](server.md) guide.

## Paths

| Path | When |
|------|------|
| [Local install](local.md) | Laptop / workstation — TUI, `--send`, loopback `--api` |
| [Secure remote API](server.md) | Ubuntu VPS — systemd + reverse proxy + Bearer tokens |

## See also

- [`philosophy.md`](../philosophy.md) — why arbiter is shaped the way it is.
- [`concepts/writ.md`](../concepts/writ.md) — the slash-command DSL agents emit.
- [`concepts/todos.md`](../concepts/todos.md) — how agents track progress on multi-step work.
- [`api/`](../api/index.md) — HTTP API reference.
- [`cli/`](../cli/index.md) — non-interactive command-line reference.
- [`tui/`](../tui/index.md) — interactive terminal client.
- [`deploy/`](../../deploy/) — systemd / Caddy / nginx templates.
