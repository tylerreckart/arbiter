# TUI engine overhaul — implementation plan

Status: **draft** · Branch: `cursor/tui-engine-overhaul` · Engine: [OpenTUI](https://github.com/anomalyco/opentui) (Zig core, C ABI)

This document is the working plan for replacing arbiter's hand-rolled C++ terminal renderer with OpenTUI's native core while preserving REPL semantics: multi-pane layout, exec-thread streaming, scrollback, multi-line input, tool-call chrome, and background loops.

---

## Goals

1. **Correctness under load** — grapheme-aware width, stable cursor, diff-based output (no full-region repaint flicker on every token delta).
2. **Performance** — native cell-buffer rendering and streaming text ingestion (`NativeSpanFeed`) for agent output; target ≤16 ms/frame on a 80×40 pane during streaming.
3. **Feature parity** — everything documented in [index.md](index.md), [panes.md](panes.md), [streaming.md](streaming.md), and [keybindings.md](keybindings.md) behaves the same or better after cutover.
4. **Multi-pane ready** — replace manual `Rect` row math with layout-driven composition so `LayoutTree` splits map to engine subtrees without rewriting chrome logic per pane.
5. **Single binary** — link OpenTUI as a static native library; **no** Bun/Node runtime in the arbiter process.

## Non-goals (v1 of overhaul)

- React/Solid reconcilers or TypeScript renderables in-process.
- Kitty graphics, WebGPU/`@opentui/three`, or audio.
- Rewriting orchestrator, SSE event model, or slash-command dispatch.
- SSH-hosted remote TUI (`@opentui/ssh`).

---

## Current architecture (arbiter)

```
main thread (pump)                exec thread (per pane)
─────────────────                 ───────────────────────
read keys → LineEditor            cmd_queue.pop → handle()
     │                                  │
     │                                  ├─ orch.send_streaming()
     │                                  │     └─ StreamFilter → OutputQueue
     │                                  └─ pane.last_response (delegation)
     │
drain OutputQueue → ScrollBuffer
render_scrollback() → raw ANSI (TUI)
ThinkingIndicator / ToolCallIndicator → TUI status rows
```

| Component | Location | Responsibility |
|-----------|----------|----------------|
| `TUI` | `include/tui/tui.h`, `src/tui/tui.cpp` | Alt-screen, fixed row layout, header/mid/hint separators, status |
| `ScrollBuffer` | `src/tui/scroll_buffer.cpp` | Visual-row scrollback, PgUp/Pn navigation |
| `LineEditor` | `src/tui/line_editor.cpp` | Input, history, slash tab-complete |
| `StreamFilter` | `src/tui/stream_filter.cpp` | Strip inline `/cmd` from streamed prose |
| `MarkdownRenderer` | `src/markdown.cpp` | ANSI styling for agent output |
| `Pane` / `LayoutTree` | `include/repl/` | Per-pane state, N-ary split tree, focus |
| `OutputQueue` / `CommandQueue` | `include/repl/queues.h` | Exec ↔ pump handoff |

~2.1k lines of TUI-specific C++. Layout is **row-index arithmetic** on a shared alt-screen; scroll region is repainted from `ScrollBuffer` on each drain.

---

## Target architecture

```
main thread (UI loop)               exec thread(s) (per pane)
────────────────────              ──────────────────────────
OpenTuiApp::poll()                unchanged command dispatch
  ├─ stdin → keymap → focus       orch.send_streaming → StreamFilter
  ├─ layout → Yoga tree               └─ SpanFeedQueue (new)
  └─ renderer.render()                  (thread-safe push)
       ↑
       └─ drain SpanFeedQueue + apply markdown spans
```

| Arbiter concept | OpenTUI primitive | Notes |
|-----------------|-------------------|-------|
| Scroll region | `TextBuffer` + `TextBufferView` in a clipped `Box` | Viewport scroll replaces visual-row buffer |
| Streaming deltas | `NativeSpanFeed` → buffer append | Batch per pump tick; avoid per-chunk full repaint |
| Markdown styling | Span attributes on feed **or** pre-render to styled spans | Evaluate parity with `MarkdownRenderer` early |
| Input area | `EditBuffer` + `EditorView` | Multi-line, 1..5 rows, history |
| Header / hints | `Text` renderables or direct buffer draw | Static + bound status text |
| Tool-call row | `Text` on mid-separator slot | Same UX as `ToolCallIndicator` |
| Thinking spinner | Status `Text` + timer in UI loop | Drop `ThinkingIndicator` thread |
| Multi-pane | Yoga layout: root → split → pane subtrees | `LayoutTree` computes flex rects; engine owns paint |
| Focus accent | Border/highlight on focused pane subtree | Replace manual separator colouring |

**Integration surface:** OpenTUI `packages/core/src/zig/lib.zig` C exports (`createRenderer`, `createTextBuffer`, `createEditBuffer`, `createEditorView`, `createNativeSpanFeed`, …) wrapped by a thin C++ RAII layer under `src/tui/opentui/`.

---

## Build and dependency strategy

### Phase 0 deliverable: vendored native lib

1. Add `third_party/opentui/` as a **git submodule** (or documented path to `../opentui`) pointing at a **pinned commit** of anomalyco/opentui.
2. CMake option `ARBITER_OPENTUI_ROOT` (default: submodule path).
3. Custom target `opentui_core` — invokes `zig build` from `packages/core/src/zig` with:
   - `-Dtarget=<triplet>` matching arbiter's platform
   - Static library output (`libopentui.a` / `.lib`)
4. Link `opentui_core` into `arbiter`; expose headers via `include/opentui/` (generated or copied C header from Zig `@cImport` shim).
5. CI: install Zig (version from opentui's `.zig-version`); cache Zig artifact directory.

**Fallback:** prebuilt platform packages (same tarballs `@opentui/core-*` publishes) downloaded at configure time if Zig is absent — optional, later.

### C++ wrapper layout (new code)

```
include/tui/opentui/
  engine.h          # OpenTuiEngine — renderer + terminal lifecycle
  handles.h         # Typed wrappers for NativeHandle
  span_feed.h       # Streaming queue adapter
src/tui/opentui/
  engine.cpp
  handles.cpp
  span_feed.cpp
  ffi_shim.c        # If needed for callback trampolines
```

Old `TUI` / `ScrollBuffer` / `LineEditor` remain until parity; compile-time flag `ARBITER_TUI_ENGINE=legacy|opentui` for incremental migration.

---

## Threading model

| Thread | Today | Target |
|--------|-------|--------|
| Main / pump | Keyboard, scroll, repaint | **UI loop**: poll input, drain feeds, `renderer.render()`, layout on resize |
| Exec (per pane) | `send_streaming` callbacks | Unchanged; callbacks push into **lock-free or mutexed** `SpanFeedQueue` per pane |
| ThinkingIndicator | Background animation thread | **Remove** — animate in UI loop from monotonic clock |
| Title generator | Async thread → `TUI::set_title` | Push title string to pane; UI loop applies to header renderable |

**Rule:** only the UI thread calls OpenTUI render APIs. Exec threads never touch handles.

**Cancellation / SIGINT:** preserve existing `orch.cancel()` path; UI loop checks interrupt flag between frames.

---

## Migration phases

### Phase 0 — Spike (1–2 days)

**Exit criteria:** standalone binary or `arbiter --tui-spike` renders alt-screen box + static text + clean shutdown on resize.

- [ ] Submodule + CMake build of `libopentui`
- [ ] C++ `OpenTuiEngine` wrapping `createRenderer` / `destroyRenderer`
- [ ] Log callback bridged to stderr (OpenTUI has no console in raw TTY)
- [ ] Document Zig version pin in this file

### Phase 1 — Scrollback replacement (1 week)

**Exit criteria:** agent turn output appears in OpenTUI scroll view; PgUp/PgDn work; scroll-while-streaming behaves like today.

- [ ] `PaneScrollView` owns `TextBuffer` + `TextBufferView`
- [ ] `OutputQueue` drain → `NativeSpanFeed` append (batch on pump)
- [ ] Port visual-row scroll offset semantics to view line index
- [ ] `new_while_scrolled` badge behaviour preserved
- [ ] Feature flag: `--tui-engine=opentui` uses new scroll path only; legacy input

### Phase 2 — Input replacement (1 week)

**Exit criteria:** multiline input, history, slash tab-complete, `\` continuation, Enter to submit unchanged.

- [ ] `EditBuffer` + `EditorView` replace `LineEditor` for opentui mode
- [ ] Key events routed through arbiter keymap (reuse existing binding table where possible; evaluate `@opentui/keymap` later as optional)
- [ ] Submit hands line to `cmd_queue` same as today
- [ ] Hint row rendered as static footer text (hidden in multi-pane)

### Phase 3 — Chrome and indicators (3–5 days)

**Exit criteria:** header, separators, tool-call row, thinking status, token stats match legacy screenshots.

- [ ] Header: agent name, title, status/stats renderables
- [ ] Mid-separator + tool-call indicator (no fighting with header — same split as today)
- [ ] Remove `ThinkingIndicator` thread; spinner in UI loop
- [ ] Focused-pane accent on split separators via layout pass

### Phase 4 — Multi-pane layout (1–2 weeks)

**Exit criteria:** `Ctrl-w v/s`, focus cycle, close pane, minimum sizes — all pass manual checklist in [panes.md](panes.md).

- [ ] Map `LayoutTree` rects to Yoga nodes (one subtree per leaf)
- [ ] `resize()` → recalculate layout → update renderer root bounds
- [ ] Border rendering between splits (replace `render_borders()` ANSI hacks)
- [ ] Per-pane independent scroll views and input editors
- [ ] `/pane` spawn: new leaf gets fresh subtree wired like today

### Phase 5 — Markdown and StreamFilter (3–5 days)

**Exit criteria:** code blocks, bold, links in agent output render equivalently; `/cmd` lines never appear in scrollback.

- [ ] Keep `StreamFilter` on exec thread **before** queue push (unchanged)
- [ ] Either feed markdown AST as styled spans into `NativeSpanFeed`, or render markdown to ANSI spans in C++ then feed — **spike both in Phase 1** and pick winner
- [ ] Regression: compare rendered output for representative agent transcripts

### Phase 6 — Cutover and cleanup (3–5 days)

**Exit criteria:** default build uses OpenTUI; legacy code deleted; docs updated.

- [ ] Default `ARBITER_TUI_ENGINE=opentui`
- [ ] Delete `src/tui/tui.cpp`, `scroll_buffer.cpp`, `line_editor.cpp` (or move to `legacy/` for one release)
- [ ] Update [index.md](index.md), [streaming.md](streaming.md) implementation notes
- [ ] Performance bench: stream 10k-token response, measure frame time p95

---

## API mapping reference

### Renderer lifecycle

```text
createRenderer(width, height, useAlternateScreen, …) → handle
rendererRender(handle)                             → diff to stdout
rendererResize(handle, w, h)
destroyRenderer(handle)
```

Screen mode: start with **`alternate-screen`** (parity with `TUI::enter_alt_screen()`). Evaluate **`split-footer`** later if we want host scrollback preservation.

### Streaming path (proposed)

```text
exec:  StreamFilter → MarkdownSpanBuilder → SpanFeedQueue.push(spans)
pump:  while queue.pop(batch) → nativeSpanFeedWrite(handle, …)
       editorView + scrollView mark dirty
       rendererRender()
```

### Pane state additions

```cpp
// include/repl/pane.h (future)
struct Pane {
    // … existing …
    std::unique_ptr<PaneScrollView> scroll;   // opentui mode
    std::unique_ptr<PaneInputEditor> input;   // opentui mode
    SpanFeedQueue                   feed_queue;
};
```

---

## Testing strategy

| Layer | Approach |
|-------|----------|
| Zig core | Rely on opentui's `zig build test` in CI submodule check |
| C++ FFI wrapper | Unit tests with `OTUI_NO_NATIVE_RENDER=1` equivalent (mock stdout capture) where feasible |
| Scroll / input | Scripted stdin + golden ANSI snapshots (doctest + pty optional) |
| Integration | Manual checklist + existing REPL flows (`/agents`, `/loop`, `/pane`, PgUp) |
| Performance | Benchmark harness: replay captured SSE text deltas through SpanFeed |

Add `tests/test_opentui_engine.cpp` in Phase 0.

---

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| Zig toolchain burden for contributors | Document install; optional prebuilt libs; CI always builds native |
| Markdown parity gaps | Keep legacy renderer path until visual diff passes |
| Threading bugs (render off exec thread) | Assert in debug builds; code review rule; TSAN on CI job |
| OpenTUI API churn | Pin submodule commit; minimal wrapper surface |
| Multi-pane resize bugs | Port layout tests; compare rect output legacy vs Yoga |
| Terminal compatibility | Test Terminal.app, iTerm2, Kitty, tmux; fall back to legacy flag |

---

## Open questions

1. **Submodule vs copy:** submodule at pinned tag (preferred) or periodic vendor sync?
2. **Markdown path:** native span attributes vs pre-ANSI feed — decide in Phase 1 spike.
3. **Keymap:** port arbiter bindings to OpenTUI key events manually, or adopt `@opentui/keymap` data format without TS runtime?
4. **Minimum terminal size:** enforce same floors as `LayoutTree` today; document changes if Yoga layout differs.
5. **Session restore:** scroll position + input buffer in session JSON — scope for Phase 6 or follow-up?

---

## References

- OpenTUI repo: `~/dev/opentui` (local clone)
- Core Zig entry: `packages/core/src/zig/lib.zig`
- Renderer docs: https://opentui.com/docs/core-concepts/renderer
- Arbiter TUI docs: [docs/tui/](index.md)
- Prior art: OpenCode production deployment (same engine)

---

## Changelog

| Date | Author | Change |
|------|--------|--------|
| 2026-07-03 | — | Initial plan |
