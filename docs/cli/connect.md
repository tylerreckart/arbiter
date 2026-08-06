# `--connect` — remote TUI client

Open the interactive TUI as a **thin client** of a remote [`arbiter --api`](api.md) process. The terminal runs locally; orchestration, conversations, agents, and provider keys live on the API host.

```bash
arbiter --connect https://arbiter.example.com --token atr_…
# or
export ARBITER_API_URL=https://arbiter.example.com
export ARBITER_API_TOKEN=atr_…
arbiter --connect
```

Local in-process TUI (`arbiter` with no args) is unchanged and remains the default.

## Flags and env

| Flag / env | Required | Description |
|------------|----------|-------------|
| `--connect [URL]` | yes (flag) | Enter remote client mode. URL may be omitted when `ARBITER_API_URL` is set. |
| `--token TOKEN` | no* | Tenant bearer (`atr_…`). Prefer `ARBITER_API_TOKEN` — `--token` is visible in `ps`. |
| `ARBITER_API_URL` | when URL omitted | Default base URL (`https://host` or `http://host:port`). |
| `ARBITER_API_TOKEN` | no* | Default bearer token. |
| `--theme PRESET` | no | Same theme switch as the local TUI. |
| `--no-exec` | no | Accepted for argv parity; remote mode never runs host `/exec` on the client. |

\*Current single-tenant `arbiter --api` accepts unauthenticated runtime routes. A token is still sent when provided (forward-compatible with multi-tenant / reverse-proxy auth). Startup fails with a readable error on `401` / `403`.

## Startup sequence

1. `GET /v1/health` — fail fast if unreachable or bad URL.
2. `GET /v1/conversations` — auth probe; fail fast on `401` / disabled tenant.
3. Reuse the newest conversation, or `POST /v1/conversations` when the list is empty.
4. Enter the normal TUI loop bound to that conversation.

The session sidebar title shows `Remote · <host>[:port]` (and the tenant name when the stream reports it). `/status` prints the same connection details.

## Wire protocol

Turns post to [`POST /v1/conversations/:id/messages`](../api/conversations/messages-post.md) and render the SSE catalog ([SSE events](../concepts/sse-events.md)) through the same `StreamRenderer` / scrollback path as local mode. Esc / interrupt calls [`POST /v1/requests/:id/cancel`](../api/requests-cancel.md) using the `request_id` from `request_received`.

Conversation list / switch / new / delete / title use the remote conversation store — not `~/.arbiter` TUI rows. Local provider keys are **not** required on the client host.

## Slash commands

UI commands that stay local: `/theme`, `/find`, `/chat list|new|switch|title|delete`, `/agents`, `/status`, `/quit`, pane chords.

Agent writs that only make sense on a host filesystem (`/exec`, local `/write`, …) return `ERR: … not available in remote (--connect) mode`. Prefer the HTTP/sandbox equivalents exposed by the remote API when available.

## Secrets

Never put the bearer token in conversation snapshots or scrollback. Prefer `ARBITER_API_TOKEN` over `--token`. The client does not write the token to disk.

## See also

- [TUI sessions](../tui/sessions.md) — local vs remote conversation stores
- [TUI panes](../tui/panes.md) — shared tenant identity when remote
- [Philosophy §3](../philosophy.md) — streaming as the wire format
- [`arbiter --api`](api.md)
