# Panes

Multi-pane layouts let you run several conversations in parallel inside the same terminal. Each pane has its own agent, history, scrollback, in-flight turn, and command queue. Layouts are an N-ary tree of leaves (panes) and split nodes (vertical or horizontal containers), mutated only through chord keys and the `/pane` command.

## Single-pane default

Launching `arbiter` opens one pane covering the whole terminal. The hint row at the bottom shows `esc interrupt · pgup/dn scroll · /agents · /help`. Plain text goes to the focused (only) pane's current agent.

## Splitting

| Chord  | Effect                                                           |
|--------|------------------------------------------------------------------|
| `^W v` | Vertical split — focused pane becomes left, new pane on the right. |
| `^W s` | Horizontal split — focused pane becomes top, new pane on the bottom. |

Splits divide the focused pane's rect equally. New panes get the `index` master agent by default; switch with `/use <agent>`. The hint row is hidden once the layout has more than one pane (clutter on every pane); the two rows stay reserved as blank padding so input-row positions don't shift between modes.

Splitting twice in the same orientation does **not** wrap a new node — the new sibling is appended to the existing split, so N panes share `1/N` each. Splitting in the other orientation wraps the focused leaf in a fresh 2-child node.

## Focus

| Chord                | Effect                                                |
|----------------------|-------------------------------------------------------|
| `^W w` / `^W ^W`     | Cycle focus to the next pane (pre-order traversal).   |

Exactly one pane is focused at any moment. The focused pane's bottom border draws with an accent colour; non-focused panes show a plain header separator and a dim placeholder prompt on their input row (so they read as "input surface, currently idle" rather than half-drawn).

PgUp/PgDn scroll the **focused** pane. Esc cancels the **focused** pane's in-flight turn — siblings keep streaming.

## Closing

| Chord  | Effect                                                                         |
|--------|--------------------------------------------------------------------------------|
| `^W c` | Close the focused pane. Last remaining pane cannot be closed.                  |

Close is graceful: the pane's command queue is stopped (so its exec thread's `pop()` returns), the exec thread is joined (which can block until the in-flight agent turn finishes — `Esc` first if you don't want to wait), and only then is the Pane destroyed. Pending output for that pane is dropped.

When closing collapses a split node to a single child, the child takes the parent's slot in the tree (no orphan single-child split nodes). Focus moves to the nearest leaf.

## `/pane` — programmatic spawn

`/pane <agent> <msg>` spawns a new pane running `<agent>` with `<msg>` as its first input. Differs from `^W v/s` in three ways:

1. The new pane gets a specific starting message instead of an empty prompt.
2. The result of that first message flows back to the spawning pane as a `[PANE RESULT]` message when the child completes (the spawner can keep typing while the child works).
3. Layout: the new pane is appended as a sibling using the spawner's parent's orientation, or wraps the spawner in a 2-child vertical split if the spawner is the root.

Agents themselves can emit `/pane <agent> <msg>` in their replies. The orchestrator parses it and the layout pane spawn fires the same way — child runs, result comes back as a tool result. Useful when the master agent wants to fan out work to a subagent without blocking its own turn.

## Independence and shared state

Independent per-pane:
- Agent and current model
- Conversation history
- Scrollback ring
- In-flight turn (one at a time per pane; queue subsequent inputs)
- Status bar, indicators
- Token counter

Shared across all panes:
- Tenant identity (when running against a remote API)
- Agents catalogue (`/create` / `/remove` affects every pane immediately)
- Loops (`/loops`, `/log`, etc. operate on the global loop manager)
- The shared scratchpad (`/mem shared`)
- Session restore key (one snapshot per cwd, see [sessions.md](sessions.md))

## Limits

- No keyboard shortcut to resize a split asymmetrically. Children share their parent's dimension equally; if you want a small reader pane next to a wide writer pane, the chord-based layout doesn't express it.
- No "zoom" / temporary maximise.
- No drag-to-resize (terminal mouse events are not consumed).
- Layout is not persisted — relaunching restores the focused pane's session (history, agent, conversation state) but always starts as a single pane. Sessions only restore content; layouts are ephemeral.
