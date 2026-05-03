# Slash commands

Every command is a single line starting with `/`. Type `/help` in any pane to dump this list inline. Commands target the **focused pane's agent** unless they take an explicit `<agent>` argument.

Plain text (no leading `/`) is treated as a message to the focused pane's current agent.

## Conversation

| Command                    | Effect                                                                 |
|----------------------------|------------------------------------------------------------------------|
| `<text>`                   | Send `<text>` to the focused pane's current agent.                     |
| `/send <agent> <msg>`      | Send `<msg>` to a specific agent (regardless of which pane is focused). The reply renders in the focused pane. |
| `/ask <query>`             | Shorthand for `/send index <query>` — the built-in master agent.       |
| `/use <agent>`             | Switch the focused pane's current agent. Subsequent plain-text input goes to this agent. |

## Agents

| Command                        | Effect                                                              |
|--------------------------------|---------------------------------------------------------------------|
| `/agents`                      | List loaded agents (id, model, color, role line).                   |
| `/status`                      | System status: focused pane, in-flight turn, queue depth, loops.    |
| `/tokens`                      | Full token + cost breakdown: per-agent and total since process start. |
| `/create <id>`                 | Create an agent with default config; opens `~/.arbiter/agents/<id>.json` for edit. |
| `/remove <id>`                 | Remove an agent (deletes the JSON; history in memory is dropped).   |
| `/reset [id]`                  | Clear an agent's conversation history. Default target is the focused pane's agent. |
| `/compact [id]`                | Summarize and clear history. Keeps a system-message synopsis at the head of the new context. |
| `/model <agent> <model-id>`    | Change an agent's model at runtime without editing the JSON.        |

## Panes

Each pane is an independent conversation view. See [panes.md](panes.md) for the full layout model.

| Command / chord            | Effect                                                                  |
|----------------------------|-------------------------------------------------------------------------|
| `/pane <agent> <msg>`      | Spawn a child pane running `<agent>` with `<msg>` as its first input. The result flows back to the spawner pane as a `[PANE RESULT]` message when the child completes. |
| `Ctrl-w v`                 | Split the focused pane vertically (children side-by-side).              |
| `Ctrl-w s`                 | Split the focused pane horizontally (children stacked).                 |
| `Ctrl-w w` / `Ctrl-w Ctrl-w` | Cycle focus to the next pane (pre-order traversal).                   |
| `Ctrl-w c`                 | Close the focused pane. The pane's exec thread is joined cleanly; in-flight turn is cancelled. |

## Background loops

A loop runs an agent repeatedly with its own buffered output, decoupled from any pane. See [streaming.md](streaming.md) for how loop output reaches the foreground.

| Command                        | Effect                                                              |
|--------------------------------|---------------------------------------------------------------------|
| `/loop <agent> <prompt>`       | Start a background loop. Returns a loop id immediately.             |
| `/loops`                       | Table of running / suspended loops with their last activity.        |
| `/log <loop-id> [last-N]`      | Print the buffered output from a loop. Default: last 50 lines.      |
| `/watch <loop-id>`             | Tail a loop's output live in the focused pane. Press Enter to detach. |
| `/kill <loop-id>`              | Stop a loop; its exec thread is joined.                             |
| `/suspend <loop-id>`           | Pause a loop after its current iteration finishes.                  |
| `/resume <loop-id>`            | Resume a suspended loop.                                            |
| `/inject <loop-id> <msg>`      | Insert `<msg>` into a running loop's input as the next iteration's prompt. |

## Fetch + memory

| Command                        | Effect                                                              |
|--------------------------------|---------------------------------------------------------------------|
| `/fetch <url>`                 | Fetch the URL, strip to readable text, send the result to the focused agent as a tool result. Uses libcurl directly — no shell injection surface. |
| `/mem write <text>`            | Append `<text>` to the focused agent's persistent scratchpad (`~/.arbiter/memory/<agent>/notes.md`). |
| `/mem read`                    | Load the agent's scratchpad into the next turn's context.           |
| `/mem show`                    | Print the raw scratchpad file inline.                               |
| `/mem clear`                   | Delete the scratchpad file.                                         |
| `/mem shared write <text>`     | Write to a pipeline-shared scratchpad visible to every agent in this conversation. |
| `/mem shared read`             | Read the shared scratchpad.                                         |
| `/mem shared clear`            | Clear the shared scratchpad.                                        |

The structured memory graph (typed nodes + relations, FTS-ranked search) is exposed via the HTTP API only — see [`docs/api/concepts/structured-memory.md`](../api/concepts/structured-memory.md). The TUI's `/mem` operates on the simpler per-agent scratchpad files.

## Plans

| Command                        | Effect                                                              |
|--------------------------------|---------------------------------------------------------------------|
| `/plan execute <path>`         | Execute a planner-produced plan file (sequence of `/cmd` lines). Each step's output streams into the focused pane. |

## Session

| Command                        | Effect                                                              |
|--------------------------------|---------------------------------------------------------------------|
| `/verbose [on\|off]`           | Toggle raw `/cmd` line streaming. Off (default): tool-call lines are swallowed and replaced by the spinner on the mid-separator. On: every `/fetch`, `/exec`, `/agent`, `/mem` line lands in the scroll region as the agent emits it. |
| `/help`                        | Print this command reference.                                       |
| `/quit` / `/exit` / `/q`       | Save the session snapshot and exit.                                 |

## Notes on argument parsing

- Commands are line-oriented: a slash command and its arguments are everything between the leading `/` and the next newline.
- There's no quoting layer above whitespace splitting. `/send research analyze the q3 report` works; `/send research "analyze the report"` would pass the literal quotes through to the agent.
- Tab completion is enabled for the leading slash command word. After the command, completion is unconfigured (just inserts a literal space).
- Unknown commands print `Unknown command. /help for list.` and don't consume the line as a message — type it again without the slash if you meant it as input.
