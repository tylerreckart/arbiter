# TUI output styling & spacing plan

Analysis of how Arbiter renders streamed output, and a phased plan to
improve overall styling, spacing, and output rendering.

**Status:** Phases 1–2 and 4–5 implemented; Phase 3 largely done for
interactive REPL emit (`push_prose` / StyleId for slash-command status,
banners, confirms, tool summaries, user echo). Phase 0 fixture inventory
remains a follow-up. Loop log dumps (`/log`, `/watch` bodies) stay on the
ANSI `push`/`push_msg` path until logs are stored as structured prose.
Nested-theme **panel/diff surfaces** use sticky-follow so chrome-only
overlays do not wipe distinct earlier customs; **syntax colors** use
unset-only fill (layout wrappers cannot clobber nested `code_*` tokens).

## Current rendering pipeline

```
agent / tool stream
        │
        ▼
  BlockParser ──────── swallow /cmd + /write bodies (unless /verbose)
        │
        ▼
  MarkdownRenderer ─── line-oriented: headings, lists, quotes, HR,
        │               inline **/*/`/[], fenced code + ```diff
        │
        ├── prose ──► StyledLine[] ──► RenderPolicy ──► OutputQueue::push_prose
        ├── code  ──► code_open / line / close ───────► OutputQueue::push_code_*
        └── diff  ──► patch string ──────────────────► OutputQueue::push_diff
                              │
                              ▼
                    PaneScrollView segments
                      ├─ ProseSegment   (StyleId → OpenTUI highlights)
                      ├─ CodeSegment    (syntax highlight + collapse)
                      ├─ DiffSegment / NativeDiffSegment
                      ├─ TextSegment    (ANSI legacy / banners)
                      └─ BlankSegment   (1-row block separator)
                              │
                              ▼
                    OpenTUI frame (~30 ms pump)
```

Key files:

| Layer | Path |
|-------|------|
| Stream glue | `src/stream_renderer.cpp`, `include/stream_renderer.h` |
| Writ filter | `src/tui/block_parser.cpp` |
| Markdown | `src/markdown.cpp`, `include/markdown.h` |
| Policy | `src/render_policy.cpp`, `include/render_policy.h` |
| Queue / block flags | `src/repl/queues.cpp`, `include/repl/queues.h` |
| Scroll composition | `src/tui/opentui/pane_scroll_view.cpp` |
| Style → color | `src/tui/style_resolver.cpp`, `include/styled_text.h` |
| Design tokens | `include/tui/tui_design.h`, `themes/*.json` |
| ANSI Theme shim | `src/theme.cpp`, `include/theme.h` |
| Pane chrome | `src/tui/opentui/pane_frame.cpp`, `include/tui/tui.h` |
| Diff UI | `src/tui/opentui/diff_panel.cpp` |

Policies already in use:

- `kMasterStream` — live master turn (8-line code preview, no writs)
- `kVerbose` — show writs / tool bodies
- `kInterim` — sub-agent: dim base style, collapse fences, max 8 rows / 480 cols
- `kReplay` — conversation switch (same numbers as master today)

## What works well

1. **Structured scrollback.** Prose / code / diff are real segments, not a
   flat ANSI dump. Code collapse and side-by-side diffs are first-class.
2. **Theme-driven colors.** `TuiDesign` + JSON presets cover chrome,
   markdown roles, syntax tokens, and agent palette. `/theme` rethemes
   live scrollback via `PaneScrollView::retheme()`.
3. **Span path for master stream.** `StyleId` → `resolve_style` → OpenTUI
   highlights avoids ANSI round-trips on the happy path.
4. **Horizontal layout tokens.** `pane_padding_x`, `header_padding_x`,
   `input_padding_x`, `footer_gap`, `compact_cols` / `dense_cols` already
   exist and are theme-overridable.
5. **Quiet default.** Tool calls collapse to mid-separator spinner + one
   summary line; scroll stays readable.

## Pain points

### 1. Dual output paths (styled vs ANSI)

Master streaming uses `push_prose` / `Code` / `Diff`. Many other emitters
still concatenate ANSI from `theme()` and call `push_msg` / `push`
(`TextSegment`):

- User echoes (`main.cpp`, transcript replay)
- Advisor / loop banners (`loop_manager.cpp`, advisor halt)
- Some command results and status lines

`TextSegment` goes through `AnsiScrollAppender`; prose goes through
`SpanScrollAppender`. Same semantic roles, two paint paths — harder to
keep spacing and color consistent, and ANSI strings bake colors at emit
time (retheme only works if re-parse succeeds).

### 2. Vertical rhythm is ad-hoc

Block separation is a single `BlankSegment` (exactly one row) inserted by
`start_block()` when `new_block` is set or before code/diff. There are
**no** theme tokens for:

- message / turn gap
- gap before/after code or diff panels
- paragraph density inside prose
- chrome vertical insets (header ↔ scroll ↔ input)

Markdown blank lines become empty `StyledLine`s (source newlines preserved),
so agent verbosity directly controls density. Message boundaries and
panel boundaries both use the same one-row blank — no hierarchy.

### 3. Diff panel colors are hard-coded

`diff_panel.cpp` `bg_for_line()` uses fixed RGB (`#0d3316`, `#4a1212`,
`#181818`, `#101010`) instead of `TuiDesign`. Light / pastel themes get
diff panels that ignore the preset. Add/remove *foregrounds* are themed;
backgrounds are not.

### 4. Code panels reuse chrome backgrounds

`CodeSegment` header uses `bg.header`, body uses `bg.scroll`, summary
uses `bg.panel`. There is no `content.code_bg` / `code_gutter` /
`code_header` token, so code blocks never get a distinct recessed
surface independent of pane chrome.

### 5. Layout density is binary

When `cols <= dense_cols` (default 88), horizontal padding snaps to `0`.
No intermediate step — wide terminals feel padded, mid-width panes go
flush. Vertical chrome (`kBottomPadRows = 3`, reserved footer when
hidden in multi-pane) is compile-time, not themeable.

### 6. Markdown surface gaps

- Strikethrough (`~~…~~`) → `StyleId::Strike` (done)
- Horizontal rules rewrite on wrap-cols change (done); markdown still emits
  a 60-dash seed string that the scroll view resizes
- No tables, task lists, or nested list indent beyond bullet glyph swap
- Indented code blocks stay as prose+`StyleId::Code` (not `CodeSegment`)
- Fence languages normalize for highlight, but the panel title is bare
  lang text with no border / file-name affordance

### 7. Measurement inconsistency

Chrome frames and scroll trim now share `display_width` /
`trim_to_display_cols` (wcwidth mode 0). Remaining risk is any new local
`cell_width` helper that reintroduces lead-byte counting.

### 8. Interim / system copy is hard-wired

Truncation strings (`… (fenced block) …`,
`… [truncated — full result in synthesis turn]`) and tool-summary glyphs
are string literals. Fine for v1; they should eventually share a small
copy/style helper so dim/prefix spacing matches user-echo and status
lines.

### 9. Chrome vs content visual hierarchy

Post–0.5.0 chrome is intentionally minimal (no pane header row; identity
lives in the top session header). Scroll content, code panels, and
diffs compete for the same visual weight because:

- Prose has no left gutter / role marker
- Code and diff both use heavy full-width panels
- System lines (`[interrupted]`, tool summary) sit in the same flow as
  model prose with only StyleId color to distinguish them

## Design principles for the work

1. **One paint path for scroll content.** Prefer `StyledLine` +
   `StyleId` (or new role ids) over ANSI string assembly. Keep `theme()`
   for non-TUI sinks (loop logs, CLI) only.
2. **Theme owns surfaces, not just foregrounds.** Diff/code backgrounds,
   gutters, and block gaps must be tokens.
3. **Vertical rhythm is a layout concern.** Message gap ≠ panel gap ≠
   chrome inset; expose them in `TuiDesign::Layout`.
4. **Density scales continuously.** Prefer clamp/lerp over on/off at
   `dense_cols`.
5. **Preserve quiet defaults.** Verbose mode and interim collapse stay;
   polish must not re-introduce tool-call noise into scroll.

## Proposed token additions

Extend `TuiDesign` / theme JSON (all optional with OneDark-derived
defaults):

```jsonc
{
  "layout": {
    // existing…
    "block_gap": 1,           // BlankSegment rows between messages
    "panel_gap": 1,           // before/after code & diff
    "prose_paragraph_gap": 1, // blank StyledLines collapsed to this max
    "scroll_pad_y": 0,        // top/bottom inset inside scroll region
    "chrome_compact_rows": false
  },
  "content": {
    // existing…
    "code_bg": "#21252b",
    "code_header_bg": "#2c323c",
    "code_gutter": "#5c6370",
    "diff_bg_context": "#181818",
    "diff_bg_add": "#0d3316",
    "diff_bg_remove": "#4a1212",
    "diff_bg_empty": "#101010",
    "system_fg": "#5c6370"    // [interrupted], tool summary rest, status prose
  }
}
```

Add `StyleId::Strike`, `StyleId::System`, `StyleId::UserEchoArrow` /
`StyleId::UserEchoText`.

## Phased plan

### Phase 0 — Baseline & fixtures (no visual ambition)

- Inventory call sites of `push_msg` / `push(` / `theme().` that feed
  pane scroll; classify: keep-ANSI (CLI) vs migrate-to-prose (TUI).
- Snapshot tests (or golden PTY fixtures) for: prose+heading, collapsed
  code, expanded code, split diff, tool summary, user echo, interim
  truncation. Gives a regression net for later phases.
- Document the segment model in `docs/tui/streaming.md` (link from this
  plan once implemented).

**Exit:** list of migrate targets; fixtures green on current look.

### Phase 1 — Theme-complete surfaces

- Move `DiffPanel` backgrounds into `content.diff_bg_*` tokens; derive
  sensible defaults per preset (especially `light`).
- Add `code_bg` / `code_header_bg` / `code_gutter` and use them in
  `CodeSegment::draw`.
- Wire tokens through `tui_design.cpp` load/export so
  `--export-theme` / `/theme save` round-trip.

**Exit:** light theme diffs and code panels no longer look “dark mode
stuck on”; custom themes can retint panels.

### Phase 2 — Vertical rhythm & spacing

- Implement `block_gap` / `panel_gap` in `PaneScrollView::start_block`
  (parameterize blank count; optionally distinguish message vs panel).
- Collapse runs of empty prose lines to `prose_paragraph_gap` (max) at
  append or policy time so chatty model output doesn’t double-space
  everything.
- Optional `scroll_pad_y`: leave blank visual rows at top/bottom of the
  viewport (draw-time inset, not fake segments).
- Soften dense-mode: e.g. `pad = min(pane_padding_x, max(0, (cols - compact_cols) / N))`
  instead of hard zero at `dense_cols`.

**Exit:** turns and panels read as distinct blocks; mid-width terminals
keep a little air.

### Phase 3 — Unify TUI emit path

- Migrate user echo, `[interrupted]`, advisor/loop banners, and tool
  summaries to `push_prose` / `push_prose_msg` with `StyleId::UserEcho`
  / `System` / existing Success|Error.
- Prefer `push_prose_msg` over ANSI `push_msg` in `main.cpp` command
  paths that target the focused pane.
- Keep `theme()` + ANSI for: non-TUI loop log buffers, CLI helpers,
  `render_diff_ansi` fallback only.

**Exit:** `/theme` recolors system lines and user echoes without
re-parse; TextSegment usage in interactive scroll shrinks sharply.

### Phase 4 — Output rendering polish

- Width-aware HR (`min(content_w, 60)` or full content width with
  `StyleId::Rule`).
- `StyleId::Strike` + attr bit (or dim+strike via OpenTUI attrs).
- Code panel header: language + optional truncation hint on the same
  row; subtle left accent using `accent.secondary` or `border.subtle`.
- Diff header row: use themed `diff_file` / panel header bg consistently
  with code panels (shared “panel chrome” helper).
- System lines: optional quiet prefix (`·` / dim rule) so they don’t
  read as model prose.
- Align measurement: reuse `display_width` (or one shared cell-width
  helper) in chrome footer, code trim, and diff truncate.

**Exit:** markdown and panels feel like one product; no more 60-col HR
on a 120-col pane.

### Phase 5 — Chrome & density ✅

- Themeable chrome vertical budget via `chrome_compact_rows` (default
  `true`): multi-pane / `show_footer: false` reclaim blank hint rows
  (`tui_bottom_pad_rows` → 1 instead of 3).
- Shared helpers: `tui_pane_pad_x`, `tui_pane_edge_pad`, `tui_input_pad_x`
  used by pane chrome, input editor, scroll bind, and both sidebars.
- `scroll_pad_y` draw-time inset and optional `scroll_gutter_cols` (default
  `0`, paints `bg.gutter`) for quiet content indent without per-role noise.

**Exit:** multi-pane and narrow terminals waste less vertical space;
padding logic lives in one helper.

## Non-goals (this effort)

- Replacing OpenTUI or reintroducing a pane header row
- Full CommonMark / GFM (tables can be a later spike)
- Persisting scrollback across process restarts
- Asymmetric pane resize / zoom (tracked separately in panes docs)
- Changing default verbose/tool-call UX

## Suggested implementation order in code

1. `tui_design` token plumbing + theme JSON defaults (Phase 1)
2. `diff_panel.cpp` / `CodeSegment::draw` consume new tokens
3. `PaneScrollView` gap helpers (Phase 2)
4. Emit-site migrations in `main.cpp`, replay, indicators (Phase 3)
5. Markdown / HR / strike / shared `cell_width` (Phase 4)
6. Chrome compaction + pad helper dedup (Phase 5)

## Test plan

| Area | Existing hooks | Add |
|------|----------------|-----|
| Theme JSON | `tests/test_tui_theme_json.cpp` | new content/layout keys round-trip |
| Markdown / diff | `tests/test_markdown_diff.cpp` | HR width, strike, paragraph collapse |
| Stream filter | `tests/test_stream_filter.cpp` | unchanged behavior under new gaps |
| Sidebar | `tests/test_sidebar_tui.cpp` | only if pad helper shared |
| Scroll / replay | `tests/test_transcript_replay_tui.cpp` | user-echo as prose; gap markers |
| Highlighter | `tests/test_code_highlighter.cpp` | code panel token smoke if exported |

Prefer unit tests around `MarkdownRenderer`, `RenderPolicy`, and
`OutputQueue` block flags before PTY harness work.

## Success criteria

- Light and dark presets both produce readable code + diff panels.
- Message boundaries and fenced panels have consistent, themeable gaps.
- Interactive scroll content is almost entirely StyleId-based; `/theme`
  recolors user echoes and system lines.
- No regression in tool-call quiet mode or code collapse (`^O`).
- Mid-width (≈80–96 col) panes keep slight horizontal padding instead of
  a hard flush.

## Open questions

1. Should `block_gap` default stay `1` (current look) or increase to `2`
   for a more “chat” feel? Recommend default `1`, let `modern` / custom
   themes opt into `2`.
2. Do sub-agent interim lines keep the two-space dim indent forever, or
   move to a gutter column once prose gutters exist?
3. Native diff view (`ARBITER_HAS_NATIVE_DIFF_VIEW`) vs `DiffPanel` —
   should Phase 1 token work target both, or only `DiffPanel` until
   native path is the default?
