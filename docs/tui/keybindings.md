# Keybindings

Arbiter ships its own line editor. Bindings below are the complete set the editor responds to. Keys not listed are inserted literally.

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
| `^W s`       | Horizontal split — focused pane split into top/bottom.                       |
| `^W w` / `^W ^W` | Cycle focus to the next pane (pre-order traversal of the layout tree).   |
| `^W c`       | Close the focused pane. Its exec thread is joined; in-flight turn is cancelled. Last remaining pane cannot be closed. |

If the chord byte doesn't match any of the above, it's silently dropped — the editor returns to the normal input state. `^W` followed by a regular character does *not* fall through to insertion; the chord window always consumes its byte.
