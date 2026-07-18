# Panes

Multi-pane layouts let you run several conversations in parallel inside the same terminal. Each pane is a viewport (vim "window") bound to a conversation (vim "buffer"): it has its own agent, scrollback, in-flight turn, and command queue. Different panes can show different conversations at once. Layouts are an N-ary tree of leaves (panes) and split nodes (vertical or horizontal containers), mutated only through chord keys and the `/pane` command.

## Single-pane default

Launching `arbiter` opens one pane covering the whole terminal, bound to the last-active conversation. The hint row at the bottom shows `esc interrupt · pgup/dn scroll · /agents · /help`. Plain text goes to the focused (only) pane's current agent.

## Splitting

| Chord  | Effect                                                           |
|--------|------------------------------------------------------------------|
| `^W v` | Vertical split — focused pane becomes left, new pane on the right. |
| `^W h` | Horizontal split — focused pane becomes top, new pane on the bottom. |

Splits divide the focused pane's rect equally. The new pane inherits the focused pane's conversation (same buffer in a new window); switch with `/chat switch` or the history sidebar. New panes use the `index` master agent by default; change with `/use <agent>`.

In multi-pane layouts the focused pane shows a compact chord hint (`^W w focus · ^W z zoom · ^W c close`); unfocused panes keep the hint row as blank padding so input positions don't shift.

Splitting twice in the same orientation does **not** wrap a new node — the new sibling is appended to the existing split, so N panes share `1/N` each. Splitting in the other orientation wraps the focused leaf in a fresh 2-child node.

## Focus

| Chord                | Effect                                                |
|----------------------|-------------------------------------------------------|
| `^W w` / `^W ^W`     | Cycle focus to the next pane (pre-order traversal).   |

Exactly one pane is focused at any moment. The focused pane's bottom border draws with an accent colour; non-focused panes show a plain header separator and a dim placeholder prompt on their input row (so they read as "input surface, currently idle" rather than half-drawn).

Unfocused panes show a small activity badge on the mid-separator when a turn is running (`●`) or when a turn completes while you were elsewhere (`✓` / `✗`). The badge clears when you focus that pane.

PgUp/PgDn scroll the **focused** pane. Esc cancels the **focused** pane's in-flight turn — siblings keep streaming.

## Zoom

| Chord  | Effect                                                                         |
|--------|---------------------------------------------------------------------------------|
| `^W z` | Toggle maximize on the focused pane. Siblings stay open; press again to restore. |

Zoom is a rendering override — the layout tree is unchanged. Cycling focus while zoomed moves the maximized pane; splitting or closing clears zoom first.

## Closing

| Chord  | Effect                                                                         |
|--------|--------------------------------------------------------------------------------|
| `^W c` | Close the focused pane. Last remaining pane cannot be closed.                  |

Close is graceful: the pane's command queue is stopped (so its exec thread's `pop()` returns), the exec thread is joined (which can block until the in-flight agent turn finishes — `Esc` first if you don't want to wait), and only then is the Pane destroyed. Pending output for that pane is dropped.

When closing collapses a split node to a single child, the child takes the parent's slot in the tree (no orphan single-child split nodes). Focus moves to the nearest leaf.

## Conversations and panes

Switching conversations (`^W b` → Enter, or `/chat switch`) attaches the selected conversation to the **focused pane only**. Sibling panes keep their conversations and the split layout stays intact. `/chat new` creates a fresh conversation on the focused pane.

Agent message histories are keyed per conversation, so two panes can stream different threads concurrently without resetting each other.

## `/pane` — programmatic spawn

`/pane <agent> <msg>` spawns a new pane running `<agent>` with `<msg>` as its first input. Differs from `^W v/h` in three ways:

1. The new pane gets a specific starting message instead of an empty prompt.
2. The result of that first message flows back to the spawning pane as a `[PANE RESULT]` message when the child completes (the spawner can keep typing while the child works).
3. Layout: the new pane is appended as a sibling using the spawner's parent's orientation, or wraps the spawner in a 2-child vertical split if the spawner is the root.

Agents themselves can emit `/pane <agent> <msg>` in their replies. The orchestrator parses it and the layout pane spawn fires the same way — child runs, result comes back as a tool result. Useful when the master agent wants to fan out work to a subagent without blocking its own turn.

## Independence and shared state

Independent per-pane:
- Bound conversation id
- Agent and current model
- Scrollback ring
- In-flight turn (one at a time per pane; queue subsequent inputs)
- Status bar, indicators
- Token counter

Shared across all panes:
- Tenant identity (when running against a remote API)
- Agents catalogue (`/create` / `/remove` affects every pane immediately)
- Loops (`/loops`, `/log`, etc. operate on the global loop manager)
- The shared scratchpad (`/mem shared`)
- Per-conversation agent histories (shared when two panes bind the same conversation)

## Limits

- No keyboard shortcut to resize a split asymmetrically. Children share their parent's dimension equally; if you want a small reader pane next to a wide writer pane, the chord-based layout doesn't express it. (Mouse drag-resize is tracked separately.)
- Layout is not persisted — relaunching restores conversations' agent histories but always starts as a single pane. Sessions restore content; layouts are ephemeral.
