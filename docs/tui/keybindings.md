# Keybindings

Arbiter ships its own line editor. Bindings below are the complete set the editor responds to. Keys not listed are inserted literally.

## Mouse (when `layout.mouse` is true)

| Action | Effect |
|--------|--------|
| Left-click pane | Focus that pane |
| Left-click input | Focus pane + move caret |
| Wheel | Scroll pane / history list under the pointer |
| Drag split gutter | Resize adjacent panes |
| Left-click history row | Switch to that conversation |
| Right sidebar | Ignored (display-only) |

Disable with `"mouse": false` under `layout` in `~/.arbiter/tui.json`.

## Editing

| Key             | Action                                                              |
|-----------------|---------------------------------------------------------------------|
| Backspace / `^H` | Delete the character left of the cursor.                           |
| `Delete`        | Delete the character under the cursor.                              |
| `^A`            | Move cursor to the start of the line.                               |
| `^E`            | Move cursor to the end of the line.                                 |
| `^B` / `←`      | Move cursor one byte left.                                          |
| `^F` / `→`      | Move cursor one byte right.                                         |
| `^K`            | Kill from cursor to end of line.                                    |
| `^U`            | Kill the entire line.                                               |
| `^W`            | Kill the previous word — but **only** if no chord handler is bound. The default REPL binds `^W` as a pane chord (see below); in that mode the previous-word kill is unavailable. |
| `Tab`           | Tab-completion on the leading word (slash commands). Inserts a literal space if no completer is registered. |
| `Enter`         | Submit the line.                                                    |
| `^D`            | EOF if the buffer is empty (exits the REPL), else delete-forward.   |
| `^C`            | Abort the current line. Does not exit the process; just discards the buffer and redraws the prompt. |
| `↑` / `↓`       | History navigation. Per-pane history persisted across sessions (see [sessions.md](sessions.md)). |

## Scrollback

| Key       | Action                                                                       |
|-----------|------------------------------------------------------------------------------|
| `PgUp`    | Scroll the focused pane's scroll region up by ~half a screen.                |
| `PgDn`    | Scroll back down. At the live tail, PgDn is a no-op.                         |
| `^O`      | Expand or collapse the truncated code block visible in the scroll region.      |

Scrollback is **visual-row aware**: a wrapped paragraph counts as multiple rows for navigation, matching what the terminal actually drew. The scrollback ring is bounded (default 20k logical lines) — older content is evicted when the buffer fills.

## Cancellation

| Key         | Action                                                                       |
|-------------|------------------------------------------------------------------------------|
| `Esc`       | Cancel any in-flight agent turn. The pane's input clears, the cancel handler pushes `[interrupted]` into scrollback, and the agent's pending HTTP request is aborted. A lone `Esc` with no follow-on within 50 ms triggers cancel; longer sequences (CSI escape codes from arrows / PgUp / etc.) are routed to the editor instead. |

## Pane chords (`Ctrl-w` prefix)

`^W` opens a 2-second window for a single follow-up byte. Inside that window:

| Chord        | Action                                                                       |
|--------------|------------------------------------------------------------------------------|
| `^W v`       | Vertical split — the focused pane is split into left/right children.         |
| `^W h`       | Horizontal split — focused pane split into top/bottom.                       |
| `^W s`       | Toggle the session sidebar (wide terminals only).                            |
| `^W w` / `^W ^W` | Cycle focus to the next pane (pre-order traversal of the layout tree).   |
| `^W c`       | Close the focused pane. Its exec thread is joined; in-flight turn is cancelled. Last remaining pane cannot be closed. |
| `^W t`       | Toggle the conversation-history sidebar (left rail). Preference is saved to `~/.arbiter/tui.json`. |
| `^W b`       | Enter the conversation sidebar for selection. Auto-shows the sidebar if hidden. Use `↑`/`↓` and `Enter`; `Esc` cancels. |

If the chord byte doesn't match any of the above, it's silently dropped — the editor returns to the normal input state. `^W` followed by a regular character does *not* fall through to insertion; the chord window always consumes its byte.

## Conversation sidebar (when focused via `^W b`)

| Key       | Action                                                                       |
|-----------|------------------------------------------------------------------------------|
| `↑` / `↓` | Move selection (`+ New conversation` at top, then prior threads newest-first). |
| `Enter`   | Switch to the selected conversation or start a new one. Scrollback clears; agent histories load from disk. |
| `Esc`     | Cancel and return focus to the pane input.                                   |

The sidebar requires at least 72 terminal columns when enabled. Below that width it auto-hides even if toggled on.
