# `arbiter --send`

One-shot, non-interactive invocation. Sends a single message to a specific agent, prints the reply on stdout, exits.

```
arbiter --send <agent> <message>
```

`<agent>` is the id of an agent under `~/.arbiter/agents/`. `<message>` is everything after that — multiple positional args are joined with spaces, so quoting is up to your shell.

## Behaviour

The agent runs the **full orchestration loop**: it can fire `/fetch`, `/exec`, `/agent`, `/mem`, `/write`, etc. internally, just as it would in interactive mode. You don't see the intermediate steps — only the final synthesised reply lands on stdout. Tool-call output is consumed by the agent, not echoed.

If the agent succeeds, the reply prints to stdout and the process exits `0`. If it fails (provider error, missing API key, unknown agent id), an `ERR: <reason>` line lands on stderr and the process exits `1`.

No TTY required. No alt-screen. No persistent session — the conversation history starts and ends with this one invocation. This is the right mode for scripts, cron jobs, CI hooks, and any pipeline where you want a deterministic-shape reply.

## Examples

Pipe input via your shell:

```bash
arbiter --send reviewer "review: if (arr.length = 0) return;"
```

Capture output:

```bash
SUMMARY=$(arbiter --send research "summarise the last week of my git log")
echo "$SUMMARY" | mail -s "Weekly summary" me@example.com
```

Compose with other tools:

```bash
git log --since='1 week ago' --oneline \
  | arbiter --send writer "Turn this changelog into release notes" \
  > RELEASE_NOTES.md
```

CI gate:

```bash
arbiter --send reviewer "$(git diff main...HEAD)" \
  | grep -q '^APPROVED' || exit 1
```

## Differences from interactive mode

| Aspect                      | Interactive (`arbiter`)                | One-shot (`--send`)                   |
|-----------------------------|----------------------------------------|---------------------------------------|
| TTY                         | Required (alt-screen)                  | Not required; works under pipes / cron |
| Session persistence         | Per-cwd snapshot                       | None — fresh history every run        |
| Tool-call output            | Streams into scrollback                | Hidden; agent consumes internally     |
| Streaming visibility        | Real-time deltas                       | Reply printed once, on completion     |
| Multi-agent panes           | Yes (`/pane`, `^W` chords)             | No — single agent per invocation      |
| Background loops            | `/loop`, `/watch`, etc.                | No                                    |
| `/cmd` slash commands       | Available                              | Not parsed — message is sent verbatim |
| Memory scratchpad reads     | Persisted across sessions              | Same disk files — agent can `/mem read` internally |

`--send` is intentionally minimal. Anything that needs streaming, queueing, or multi-agent coordination should use either the interactive mode or the [HTTP API](api.md).

## Provider keys

The same precedence as the other modes (see [environment.md](environment.md)):

1. `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` env vars (preferred for scripts).
2. `~/.arbiter/api_key` / `~/.arbiter/openai_api_key` files.

If a script can't find a key, the agent's first model call fails with `ERR: <provider-error>` and the process exits `1`. There's no interactive prompt.
