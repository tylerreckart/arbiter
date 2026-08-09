# Changelog

All notable changes to arbiter are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
loosely while pre-1.0 (breaking changes can land on minor bumps).

## [Unreleased]

### Fixed
- **SSE live-tail mailbox growth (#189).** Request-event, notification, and A2A
  resubscribe streams cap per-connection mailboxes at 2048 events; slow clients
  get a `slow_consumer` terminal instead of unbounded memory growth. A2A
  `tasks/resubscribe` overflow closes the stream with `state: working`,
  `final: true`, and `x-arbiter.error_code: slow_consumer` — not
  `TaskState::failed` — so a still-running task is not misreported.
- **TUI broken-sandbox host fallback.** When `ARBITER_SANDBOX_IMAGE` is set but
  the sandbox fails usability, the TUI now disables `/exec` (returns `ERR`)
  instead of falling through to confirm-gated host `popen`.
- **Event routing past the newest-200 agent page.** `POST /v1/events` scans
  tenant agents via `list_agent_records_for_routing` (ascending `agent_id`,
  soft-capped at 10000) rather than the REST list page.
- **Sandbox timeout survivor kill vs concurrent exec.** Same-tenant `/exec`
  is always serialized on the per-tenant mutex so timeout cleanup cannot
  SIGKILL a sibling exec when workspace quota is disabled.

### Added
- **`/map` workspace tree writ.** Cheap structural index of the conversation
  workspace (TUI) or process cwd (CLI): indented tree, skips heavy/hidden dirs,
  depth/entry/byte caps, optional `/map <subdir>`. Granted to coding starters
  (reviewer/backend/frontend/devops/planner). Prefer `/map` before `/exec`
  ls/find for layout discovery. Outline + turn inject are follow-ups.
- **Conversation-scoped workspace roots (TUI).** Each conversation's stored
  `cwd` is the host root for `/write` and `/diff apply`. Switching chats
  switches the project directory with them; missing/unknown roots refuse the
  write (no process-cwd fallback). Sidebar subtitles show the bound project
  dirname. CLI `--send` still uses process cwd.
- **Model catalog UX.** Shared `model_catalog` (id, provider, `context_window`)
  powers `GET /v1/models`, the interactive `/model` catalogue, the first-run
  wizard picks, and `context_window_for_model` used by auto-compaction and the
  TUI sidebar. `/model` with no args lists the catalogue; `/model <agent>`
  shows the current model + window; setting a model reports the window.
- **Tunable provider circuit breaker.** `ARBITER_CIRCUIT_FAILURE_THRESHOLD`
  (default 5) and `ARBITER_CIRCUIT_COOLDOWN_SECONDS` (default 30) configure the
  process-wide breaker used by `--api`.
- **CORS origin allowlist.** `ARBITER_CORS_ORIGINS` (CSV) echoes matching
  `Origin` values (with `Vary: Origin`); unset keeps permissive `*`.
- **Sandbox workspaces root env.** `ARBITER_SANDBOX_WORKSPACES_ROOT` overrides
  `~/.arbiter/workspaces`.
- **Container-side exec timeout.** Sandbox `/exec` wraps commands with GNU
  `timeout` when present; on deadline, leftover non-PID-1 processes inside the
  warm container are killed best-effort.
- **TUI opt-in Docker `/exec`.** The interactive TUI honours `ARBITER_SANDBOX_*`
  the same way `--api` does. Host `/exec` always confirms with a
  `HOST SHELL (unsandboxed)` permission card.
- **Event routing for tenant agents.** `POST /v1/events` matches
  `event_types` on file-backed agents first, then tenant agents from
  `POST /v1/agents` (stable `agent_id` order), then `index`.

### Changed
- **HTTP API multi-tenant bearer auth restored.** Runtime `/v1/*` routes again
  require `Authorization: Bearer atr_…` resolved via `TenantStore::find_by_token`.
  `/v1/admin/tenants*` create/list/get/patch are available again (admin bearer).
  `--api` no longer auto-provisions a default tenant or serves the data plane
  without a token; `--connect` requires `--token` / `ARBITER_API_TOKEN`.
- **Tenant token rotation.** `arbiter --rotate-tenant-token <id|name>` and
  `POST /v1/admin/tenants/:id/rotate-token` issue a new `atr_…` key (upgrade
  recovery from single-tenant installs whose plaintext was never shown).
  Admin HTTP rotate also cancels in-flight streams; the CLI is DB-only and
  warns that hot revoke needs the admin path.
- **Kill-switch via `TenantGate`.** Durable revoke probe: after auth, handlers
  bind a thread-safe `TenantGate` to the per-request `ApiClient` preflight
  (inherited by `/parallel` children). `alive()` re-reads disabled /
  `api_key_hash` on every provider call and mid-stream read. Admin HTTP
  disable/rotate also cancels `InFlightRegistry` via shared
  `cancel_for_tenant()`; CLI disable/rotate remain DB-only (soft revoke until
  the next preflight) and print that operators need admin HTTP for an
  immediate `cancel()`. Sticky `hard_cancelled_` survives ephemeral cancel
  clears. Admin PATCH requires boolean `disabled`. A2A kill-switch stays
  JSON-RPC-shaped.
- **Sandbox idle reaper logging.** Reaps emit structured
  `sandbox_container_reaped` via `Logger` instead of raw `fprintf`.
- **`POST /v1/events` status.** Documented as stable now that tenant-agent
  routing is included.

## [0.11.0] — 2026-08-06

Minor release: remote TUI `--connect` thin client, API idempotency replay
hardening under concurrency, and A2A streaming `/write` aligned with
orchestrate sandbox persist.

### Added
- **Remote TUI (`--connect`).** `arbiter --connect <base-url> [--token atr_…]` (or `ARBITER_API_URL` / `ARBITER_API_TOKEN`) opens the interactive TUI as a thin client of a remote `arbiter --api`. Startup probes `/v1/health` and `/v1/conversations`, binds a remote conversation, streams `POST /v1/conversations/:id/messages` SSE into the same scrollback path as local mode, and cancels via `POST /v1/requests/:id/cancel`. Session chrome shows `Remote · host`; local provider keys are not required on the client. See [`docs/cli/connect.md`](docs/cli/connect.md).

### Fixed
- **API idempotency replay under concurrency.** Claim `Idempotency-Key` before
  opening orchestrate SSE and before inserting `request_status` so duplicate
  keys tail the winner instead of starting a second run or leaving orphan
  rows. Losers mark stale `running` rows failed, wait briefly for the winner
  row (`503` when not ready yet), and still join replay when
  `request_status` insert fails after a claim.
- **A2A streaming `/write`.** Same sandbox persist and per-response byte-cap
  rollback as orchestrate SSE.
- **Remote TUI lifecycle and catalog routing.** Cancel remote SSE turns on
  conversation switch/delete/pane close without aborting sibling panes;
  sidebar rename, `/chat` search, and `/use` hit the remote API; folder ops
  are rejected in remote mode.
- **Pane turn cancel vs global interrupt.** `cancel_pane_turn` only cancels a
  pane with an active turn token or remote SSE gate — no fallback to
  process-wide `orch.cancel()` during idle pane teardown.

## [0.10.0] — 2026-08-04

Minor release: conversation folders in the history sidebar, TUI threads
unified into `tenants.db`, 100 built-in color schemes, Mermaid docs
diagrams, plus sandbox write and sidebar hardening.

### Added
- **Conversation folders.** File chats into named folders in
  `tenants.db` (`conversation_folders`). The history sidebar shows a
  sectioned tree (New / Folders / Chats) with collapse state in
  `tui_prefs`, per-row menus (`m`), and `/chat folder
  list|new|rename|delete|move` for narrow terminals. REST:
  `GET|POST|PATCH|DELETE /v1/conversation-folders`. See
  [`docs/tui/sessions.md`](docs/tui/sessions.md).
- **TUI conversations in `tenants.db`.** `ConversationStore` now backs
  sidebar threads on SQLite so `/todo`, `/mem`, and artifacts share one
  conversation id with the HTTP API surface. Legacy
  `~/.arbiter/conversations/` JSON is imported once; `origin=tui` rows
  stay isolated from API list endpoints. See
  [`docs/tui/sessions.md`](docs/tui/sessions.md).
- **100 built-in TUI color schemes.** Expanded the embedded theme catalog
  from 38 to 100 presets, filling out known families already in-tree
  (Catppuccin Frappé/Macchiato, Tokyo Night Storm/Light, Rosé Pine Moon,
  Ayu Mirage/Light, Kanagawa Dragon/Lotus, Everforest Light, PaperColor
  Dark, Nightfox variants) plus popular editor/terminal schemes such as
  Iceberg, Sonokai, Aura, Cyberdream, Doom One, Modus, Cobalt2, and
  Tomorrow Night. Docs table and theme JSON tests updated accordingly.
- **Mermaid diagrams in self-hosted docs.** ` ```mermaid ` fences render
  client-side from a vendored bundle so `npm run serve` stays
  offline-capable; the script injects only on docs pages that contain
  diagrams.
- **High-level architecture overview.** Conceptual diagram of TUI /
  CLI / API sharing the orchestration runtime, persistence, and
  outbound integrations. See
  [`docs/concepts/architecture.md`](docs/concepts/architecture.md).

### Fixed
- **Hermetic PTY / CI provider calls.** `ARBITER_OFFLINE=1` and the
  PTY harness `dummy-key-no-network` short-circuit `ApiClient` before
  any TLS round-trip, so TUI integration tests no longer depend on
  live provider 401 latency. `/find` status paints clear-then-rewrite
  and include the match `@row` so OpenTUI cell-diff cannot drop a
  digit-only `/find next` update (the recent `chat_command_tui` flake).
- **macOS `unit_sandbox_quota` truncation seed.** The docker stub serves
  a hermetic `__ARB_TEST_OVERFLOW__` payload (sibling `overflow.dat`) and
  runs workspace commands via `sh -c` instead of `eval`, so nested-quote
  generators and bind-mount visibility cannot collapse oversized `/exec`
  output on macOS CI.
- **Sandbox `/write` file-cap and ERR responses.** Orchestrate and tool
  interceptors release the per-response file cap and return ERR when
  sandbox persist fails (including after SSE `file` emit ordering), and
  append a truncation trailer when docker exec hits `output_max_bytes`.
- **HTTP list endpoints and TUI conversation ids.** Invalid or
  `origin=tui` `conversation_id` filters on todos/memory list endpoints
  return 400 instead of falling back to tenant-wide rows.
- **Wrapped `/find` jump targets.** Search maps matches through visual
  wrap rows so jumps land on the painted line that contains the hit.
- **History sidebar pin, scroll, and empty-chat reuse.** Selection stays
  coherent after folder delete/collapse; scroll clamps to the painted
  line budget; multi-pane new-chat reuse and pin-jump scroll behave
  correctly; unused empty chats are unfiled when new-chat targets no
  folder.

## [0.9.1] — 2026-07-30

Patch release: mouse text selection in scrollback, unified interactive
prompt queue for confirms and diff reviews, plus TUI pane-close /
DiffPanel and `/write` / confirm sequencing fixes.

### Added
- **Mouse text selection in scrollback.** Drag across the output area to
  highlight text; release copies via OSC 52 (status shows character count).
  A click without a drag still expands/collapses thinking, tools, and
  truncated code. Esc clears the selection before it cancels a turn. See
  [`docs/tui/panes.md`](docs/tui/panes.md).
- **Unified interactive prompt queue (TUI).** Confirms and diff reviews share
  a FIFO queue (`InteractivePromptQueue`) so a second prompt never silently
  declines an earlier waiter. Streamed ```diff fences auto-enqueue review
  cards; keys are `[a]`pply / `[r]`eject / `[A]`llow all (session accept-edits)
  / Esc. File-add diffs render full-width without a `/dev/null` half.
  See [`docs/tui/output-ux.md`](docs/tui/output-ux.md).

### Fixed
- **TUI fatal SIGSEGV/SIGHUP on pane close and SIGABRT in DiffPanel.** Pane
  close/shutdown no longer joins exec threads while holding `layout_mu`
  (deadlock with `/pane` spawn, `/find`, or `present_all` → hung
  `pthread_join`, then SIGHUP/SIGSEGV when the terminal drops). Close also
  cancels the pane's in-flight turn (as docs promise) so join is not stuck
  on a live network call, clears child `parent_pane` links before destroy,
  and refuses to parent new `/pane` spawns onto a mid-close pane. Confirm /
  diff-review / pending-close prompts and PgUp/expand handlers mutate
  scrollback under `layout_mu` so the output pump cannot UAF `DiffSegment`
  mid-draw. Diff panel wrap invalidation no longer re-parses the patch
  every frame.
- **TUI `/write` persists to cwd.** Interactive TUI and `--send` clear the
  API capture-only write interceptor so `/write` confirms and writes the
  process cwd (verified). Diff apply also tolerates stale hunk offsets when
  context matches uniquely, collapses `a/./path` segments, and treats
  unmarked hunk lines as context.
- **Permission / confirm sequencing.** Concurrent destructive `/exec`
  confirms (and confirm + diff review) no longer overwrite each other’s
  promises with a fake decline — approved commands report success.

## [0.9.0] — 2026-07-29

Minor release: interactive `/diff` apply/reject/undo review, durable API
idempotency across restarts, LaTeX→Unicode math in the TUI, refreshed
starter constitutions, and pane / sandbox / interrupt hardening.

### Added
- **Interactive `/diff` review (TUI).** `/diff` and `/diff review [N]` prompt
  `[a]pply` / `[r]eject` / Esc on pending patches (same interrupt bridge as
  tool confirms). `/diff apply` remains a direct write. Missing target files
  are created from the patch’s new-side lines; apply is the permission grant
  (no `/write` confirm). See [`docs/tui/commands.md`](docs/tui/commands.md).
- **`/diff` apply / reject / undo (TUI).** Streamed ` ```diff ` fences
  register as pane-local `Patch #N` proposals with an action line above
  the rendered panel. `/diff apply [N]` writes the unified patch under
  the process cwd (exact hunk match; refuses absolute/escaping paths and
  stale context); `/diff undo [N]` restores the pre-image when the file
  is unchanged since apply; `/diff reject [N]` keeps the render and
  marks the proposal rejected. See [`docs/tui/commands.md`](docs/tui/commands.md).
- **Durable idempotency map.** `Idempotency-Key` mappings now persist in
  `tenants.db` (`idempotency_keys`) with the same 24h TTL. A client retry
  after an API server restart joins the original `request_id` instead of
  starting a second run. The in-process table remains an L1 cache;
  SQLite is authoritative. See [`docs/api/orchestrate.md#idempotency`](docs/api/orchestrate.md#idempotency).
- **LaTeX math → Unicode (TUI).** Markdown display math (`\[…\]` / `$$…$$`)
  and inline `\(...\)` render as terminal-friendly Unicode approximations
  (fractions, super/subscripts, `\times` / `\approx` / `\text{}`, Greek)
  instead of raw TeX. Same-line display delimiters with trailing prose
  no longer swallow the rest of the line.

### Fixed
- **Stacked pane gutters.** Inactive panes are content-only (no readline);
  only the focused pane paints an input box. Stacked gutters are a single
  separator cell (matching vertical splits): no trailing pad on mid-stack
  panes, and the one-row output float only on outer-top panes.
  `scroll_top_row` / `scroll_region_rows` follow that same `outer_top` rule
  so mid-stack viewports match the drawn output box. Focus changes that
  show/hide the readline band adjust `scroll_offset` so a scrolled
  viewport does not jump.
- **Empty sub-pane pollution on restore.** Layout restore replays each
  `(conversation_id, agent)` transcript at most once (pre-order first leaf),
  matching live `^W` splits that inherit a conversation with empty scrollback.
- **Esc/interrupt during in-progress confirm or turn (crash/hang).** Esc
  no longer races `Pane::turn_cancel` (`shared_ptr` assign vs cancel),
  drops confirm/diff wakeups by clearing `interrupt_flag_` at `read_line`
  entry, or leaves stack `promise` waiters hung so pane close deadlocks
  under `layout_mu`. Confirm/diff posts use heap promises; Esc/cancel/
  teardown always completes them; exec threads wake input via
  `active_readline` / try-lock instead of unlocked `layout.focused()`.
- **Sandbox `/exec` workspace quota (#136).** When `workspace_max_bytes` is
  set, `/exec` now holds the per-tenant quota mutex for the full docker
  exec (matching `/write`) so parallel `/write` cannot grow the workspace
  during shell commands, and a post-exec measurement fails the tool when
  shell redirects push usage over the cap.
- **Idempotency L1 rehydrate TTL.** `IdempotencyCache::get()` passes the
  same wall-clock snapshot to SQLite on L1 miss so a key cannot appear
  live in-process while the durable row is already past TTL at the
  boundary.

### Changed
- **Starter agent constitutions.** Seed agents under `agents/` now use
  current OpenRouter model slugs fitted to each role (Sonnet 5 / Opus 5,
  GPT-5.5, GPT-5.6 Sol, Gemini 3.6 Flash, Grok 4.5), expanded capability
  allowlists (`/read` `/list` `/search` `/fetch` `/browse` `/lesson`
  `/schedule` `/pane` `/advise` `/mcp` where appropriate), and tighter
  tool-use rules. Re-seed with `arbiter --init --force` to pick them up.
  Advisor-enabled starters list `/advise` so the capability gate matches
  the ADVISOR prompt block.
- **OpenRouter Claude id rewrite.** Bare `claude-sonnet-4-6`-style ids
  now map to dotted OpenRouter slugs (`anthropic/claude-sonnet-4.6`).
  Context-window estimates use the same normalization so sidebar fill
  and auto-compaction match the live 1M-class windows.
- **Models catalogue / setup wizard.** `/v1/models` and first-run picks
  list current OpenRouter ids used by the starters.

## [0.8.9] — 2026-07-26

Patch release: pane layout persistence across TUI relaunch, mid-turn
recovery, Phase 1 doc alignment, plus capability-gate and sandbox/API
race fixes.

### Added
- **Pane layout persistence (TUI, #42).** Multi-pane split trees (orientation,
  weights, focus, per-pane conversation id + agent) save to
  `~/.arbiter/conversations/layout.json` on quit and after split / close /
  focus / conversation switch / separator drag. Relaunch restores the
  arrangement and replays each pane's transcript tail. Missing or deleted
  conversation ids remap to the active conversation; corrupt snapshots fall
  back to a single pane.
- **In-flight turn recovery (TUI).** Tool-result envelopes are committed to
  agent history immediately after tools finish, and mid-turn
  `save_async` checkpoints run after each model iteration and each tool
  batch so quit/cancel/SIGKILL cannot drop completed tool work.

### Changed
- **Doc drift pass (Phase 1).** Sessions / scheduler / memory / sandbox docs
  aligned with shipped TUI parity: global `conversations/` store, TUI
  scheduler ticker, DB-backed `/mem` graph + scratchpad, sandbox idle reaper
  and host-exec flags documented. Phase 1 roadmap items and acceptance
  criteria checked.

### Fixed
- **Capability gate for /todo /schedule /lesson (#91).** Dispatcher
  `bundle_of` now maps these writs into allowlist bundles so constrained
  agents can no longer bypass `capabilities`. Starter agents list `/todo`
  so delegated specialists can mark progress when `[DELEGATION CONTEXT]`
  injects open todos; `/schedule` and `/lesson` remain opt-in.
- **Sandbox cold-start lock scope (#95).** `ensure_container` no longer
  holds the process-wide map mutex across `docker inspect` / `docker run`
  (up to ~30s). Map mutations stay under `mu_`; same-tenant start/stop
  races use a per-tenant mutex so cross-tenant cold-starts overlap.
- **Tenant kill-switch (#90).** `resolve_primary_tenant` skips disabled
  tenants; API returns `403` when all tenants are disabled; `--api`
  startup no longer auto-re-enables a disabled primary.
- **Atomic `file_max_bytes` (#92).** SSE and A2A write interceptors reserve
  bytes with `compare_exchange_weak` so parallel `/write` cannot exceed
  the per-turn file cap.
- **Sandbox workspace quota race (#129).** `write_to_workspace` holds the
  per-tenant start mutex across quota measurement and write so parallel
  children cannot overshoot `workspace_max_bytes`.

## [0.8.8] — 2026-07-25

Patch release: Phase 1 context compaction, periodic conversation autosave,
multi-pane outer-bottom alignment, plus community docs (Contributor
Covenant, issue templates) and a refreshed roadmap toward 1.0.

### Added
- **Context compaction.** Threshold-triggered summarize of older turns
  keeps full histories for replay while sending a summary envelope plus a
  recent window to the provider. Session JSON v2 persists
  `CompactionState`; `/compact [agent]` forces it; `/reset` clears it.
  HTTP replay restores the rolling summary via durable boundary tracking
  (`boundary_db_id`, with content-match fallback and gap-fold when the
  boundary falls outside the newest-100 hydrate cap). Fail-open on
  summarize errors. See `docs/tui/sessions.md`.
- **Periodic conversation autosave.** Dirty conversations flush on a
  background timer (default 30s, `ARBITER_AUTOSAVE_INTERVAL_SEC`; `0`
  disables the tick). Post-turn `save_async` remains immediate.
- **Contributor Covenant Code of Conduct.**
- **Phased roadmap toward 1.0.** Feature audit and phased `ROADMAP.md`
  (with a README pointer).

### Changed
- **Multi-conversation autosave queue.** `ConversationStore::save_async`
  keeps a per-id pending slot so multi-pane turns do not drop each other's
  saves. `/reset` and `/compact` also queue an autosave.
- **Issue templates.** Updated GitHub feature-request / issue templates.
- **Open Graph image.** Refreshed arbiter.run OG asset.

### Fixed
- **Multi-pane outer-bottom alignment.** Shared footer pad on every
  outer-bottom pane keeps column bottoms and sidebars aligned; stacked
  panes use a single trailing pad so horizontal gutters stay uniform.
  Mid-stack focus paints the chord hint on that column's outer-bottom
  pane; zoomed layouts ignore off-screen siblings for chrome budget.

## [0.8.7] — 2026-07-23

Patch release: embed built-in TUI themes in the binary and default to
`high-contrast`.

### Added
- **Embedded themes.** `themes/*.json` are compiled into the binary via
  `cmake/embed_themes.cmake` (same pattern as starter agents), so
  single-binary / `curl | sh` installs still have the full preset
  catalog. `--init` writes them from the embed table.

### Changed
- **Default TUI preset is `high-contrast`.** Replaces `onedark` as
  `kDefaultTuiPreset` for new `tui.json` files and first-run design.

## [0.8.6] — 2026-07-23

Patch release: fix macOS release configure against system libcurl, and
run the release packaging path on every PR.

### Fixed
- **macOS release CURL::libcurl generate.** Point FindCURL at the SDK
  `libcurl.tbd` instead of `/usr/lib/libcurl.4.dylib` (dyld shared cache
  has no on-disk file, which left `IMPORTED_LOCATION` unset). Harden
  CMake to repair a missing curl import location.
- **macOS portable smoke false positive.** `otool -L` prints the binary
  path under `/Users/runner/...` before dependency lines; only scan
  indented dylib entries for non-portable load paths.

### Added
- **PR portable packaging check.** After the existing Debug suite,
  `build_and_test` runs `.github/scripts/portable_release.sh` (Release
  reconfigure on macOS only; Linux reuses the Debug binary) so curl.tbd
  / OpenTUI RPATH regressions fail on the PR without extra CI jobs.

## [0.8.5] — 2026-07-23

Patch release: fix the Linux release smoke test so portable
`$ORIGIN`-resolved `libopentui` is not rejected as a CI path.

### Fixed
- **Release smoke false positive.** `ldd` prints absolute paths under
  the runner temp dir for correctly `$ORIGIN`-resolved libraries;
  require same-directory resolve instead of treating any `/home/runner`
  path as non-portable.

## [0.8.4] — 2026-07-23

Patch release: portable macOS/Linux release binaries so `curl | sh`
installs work without Homebrew curl/openssl.

### Fixed
- **Portable macOS/Linux release binaries.** Release builds no longer
  embed Homebrew (`/opt/homebrew/opt/curl`, `openssl@3`) or CI absolute
  RPATHs. macOS links system libcurl and static OpenSSL; both platforms
  ship `libopentui` next to `arbiter` with a same-directory RPATH.
  `install.sh` installs the companion library. Fixes
  `Library not loaded: .../libcurl.4.dylib` after `curl | sh` install.

## [0.8.3] — 2026-07-23

Patch release: conversation sidebar action menu, the arbiter.run
marketing/docs site and installer, plus SEO and installer robustness
fixes.

### Added
- **Conversation sidebar action menu.** Press `m` on a history row for
  Open / Rename / Delete (first-letter shortcuts `o` / `r` / `d`);
  documented vim `j`/`k`, rename, soft-delete, new conversation, and
  type-to-filter bindings.
- **arbiter.run web surface.** Marketing homepage and docs site with
  Pug templates, direct macOS binary download, and an installer that
  selects the newest published release with a compatible verified
  binary.

### Changed
- **Site SEO.** Open Graph image, structured data, richer
  sitemap/robots/llms.txt, and Google Analytics on every page.

### Fixed
- **Installer resilience.** Tag parsing from compact GitHub JSON;
  GitHub releases API failures fall through to the no-binary path
  instead of aborting under `set -e`.
- **Docs search href escaping.** Attribute-escape and validate search
  result hrefs before render/navigation.
- **Sidebar focus hint length.** Menu footer stays within the PTY
  focus-check trim budget so `/ filter` remains visible to tests.

## [0.8.2] — 2026-07-22

Patch release: theme catalog growth with an interactive picker, denser
dual-sidebar chrome, session token persistence, and wait-state spinner
unification.

### Added
- **Interactive `/theme` picker.** Browse and preview built-in and custom
  themes without leaving the session; twenty more presets ship in-tree
  (38 total).
- **Session restore.** Last conversation reloads on restart; conversation
  token totals persist with the session and surface in the history
  sidebar.

### Changed
- **Dual-sidebar gutters and chrome.** History and stats sidebars share
  tighter spacing; model labels shorten in the stats column.
- **Expandable output blocks.** Click a collapsed output segment to
  expand it in place.
- **Unified wait-state spinners.** One Braille loader with rotating wait
  phrases across thinking / tool wait chrome; output pane flush with
  readline and inset scroll content to match inter-box gaps.

### Fixed
- **Stale token totals on save.** Concurrent `add_tokens` updates are no
  longer overwritten by a pre-save snapshot.
- **Manifest vs session totals on load.** Restart prefers the max of
  manifest and `usage.total_tokens` so a stale positive manifest cannot
  hide a higher session total.

## [0.8.1] — 2026-07-21

Patch release: TUI chrome polish so live sessions and conversation
replay share the same rounded frames, echo padding, and thinking-block
rhythm.

### Changed
- **Rounded box chrome.** Readline input and history/session sidebars
  share Unicode rounded frames (`╭──╮` / `╰──╯`); the Arbiter top header
  is removed so scrollback keeps the vertical space. Status / activity
  badges paint on the input top border.
- **Thinking blocks.** Collapsed reasoning uses the same box chrome with
  a markdown body, a gap before the box, and inline ellipsis truncation
  instead of a dedicated ellipsis row.

### Fixed
- **User-echo wrap / padding.** Echo source rows stay unpadded so the
  render path owns vertical chrome; wrapping applies a single horizontal
  inset. Live and replayed echoes match.
- **Transcript replay preamble.** Orchestrator / `AGENTS` preamble is
  stripped on replay so conversation switch matches the live session
  view.

## [0.8.0] — 2026-07-20

Minor release focused on the TUI activity timeline and multi-pane
session model.  Turns render as expandable tool/thinking segments with
persisted chrome across conversation switches; panes bind independently
to conversations (zoom, activity badges, mouse); and
`arbiter --setup-tools` walks operators through search, browse, and MCP
setup.  Also hardens sandbox path escapes, A2A SSRF, MCP child env, and
a cluster of TUI/CI flakes.

### Added
- **`arbiter --setup-tools`.** Interactive OpenTUI wizard for `/search`
  (Brave key → `~/.arbiter/search_api_key`), `/browse` (Playwright MCP
  preset), and the MCP registry (`~/.arbiter/mcp_servers.json` — hosted
  `mcp-remote` or custom stdio). Offered at the end of first-run setup;
  re-runnable anytime. Search key resolution now also reads the saved
  file after the env vars.
- **Agent output UX overhaul.**  Turns render as a first-class activity
  timeline: per-tool `ToolSegment` rows (Started → Finished, expandable
  with `^O`), collapsible provider `ThinkingSegment` when reasoning
  deltas are emitted (Anthropic / OpenAI / Gemini thought parts),
  multi-line permission cards for `/write` and destructive `/exec`,
  interim sub-agent headers, and `tool_trace` + `thinking` persistence so
  conversation switch rebuilds tool and reasoning chrome.  Nested tools
  dual-write to the pane agent (for replay) and the dispatching child;
  `/parallel` workers re-pin the spawning pane for live callback routing.
  See `docs/tui/output-ux.md`.
- **Markdown polish.**  Task lists (`- [ ]` / `- [x]`), nested numbered
  lists, and indented code blocks route into `CodeSegment` when the
  stream sink is wired.
- **Pane ↔ conversation decoupling (#40).**  Each pane binds to a
  conversation id; `/chat switch` and the history sidebar attach to the
  focused pane only, leaving sibling panes and the split layout intact.
  Agent histories are keyed per conversation so concurrent panes can
  stream different threads.
- **Pane zoom (#43).**  `Ctrl-w z` temporarily maximizes the focused pane
  without closing siblings.
- **Unfocused activity badges (#41).**  Non-focused panes show `●` while a
  turn runs and `✓` / `✗` when a turn completes off-focus.
- **Multi-pane hint degradation (#47).**  Focused multi-pane layouts show a
  compact chord hint instead of hiding the footer row entirely.
- **TUI mouse support.** SGR mouse tracking (click-to-focus, wheel scroll,
  input caret placement, history-sidebar clicks, drag-to-resize splits).
  Opt out with `"layout": { "mouse": false }` in `~/.arbiter/tui.json`.

### Changed
- **Thinking blocks.** Reasoning rows render as markdown on the same
  background as user echo / readline, with matching vertical pad and
  inset, dimmed readable text, and a left accent from the theme’s
  per-agent palette.
- **Session sidebar.** Drop the always-on Task section. The MCP section
  only appears after an MCP tool has been used in the session (same
  pattern as Todos / Scheduled).

### Fixed
- **Sandbox workspace symlink escape.** `/write` and `/read` against the
  per-tenant sandbox workspace now canonicalise the target and reject
  paths that resolve outside `workspaces/t<tid>/`, so an in-workspace
  symlink cannot reach host files.
- **Host `/write` on A2A send + scheduler.** `wire_orch_tools` always
  installs a write interceptor (with `file_max_bytes` accounting and
  optional sandbox mirror) so agents no longer fall through to
  `cmd_write` on the API server cwd.
- **A2A HTTP SSRF guard.** The A2A client shares `/fetch`'s opensocket
  denylist and http(s)-only redirect protocol limits, so federation
  redirects cannot reach private/link-local/metadata addresses.
- **MCP child env credential scrub.** Stdio MCP subprocesses inherit a
  scrubbed parent environment (secret-shaped keys stripped); registry
  `env` extras remain an explicit opt-in.
- **`unit_sandbox_ssrf` TSan flake.** Path-only sandbox workspace tests
  set `idle_seconds = 0` so the idle reaper thread never starts.
- **`chat_command_tui` switch/replay flake.** Wait for each dummy-key
  turn to finish (auth error) before `/chat new` / `/chat switch`, and
  match an interior marker substring so pane-edge clipping cannot
  miss the replay assertion on macos-arm64.
- **`line_editor` Ctrl-U flake.** Warm the input row, then assert
  functionally via a post-kill `/agents` submit (rejecting a
  `garbage/agents` echo) instead of relying on a fixed `read_for` or a
  contiguous `"ok"` paint under OpenTUI cell diffs.
- **Sub-agent `/parallel` fan-out.** Same `agent_id` may appear more than
  once in a `/parallel` block again (ephemeral clones — matching the
  documented behaviour), and a sub-agent may fan out to copies of itself.
  Starter agents that already had `/agent` now list `/parallel` explicitly
  so research (and siblings) can fan out independent angles without hitting
  a hard reject.
- **TUI SIGSEGV on degenerate pane draws.** Zoom siblings and squeezed
  zero-width/height splits still went through scroll + editor paint. OpenTUI's
  `bufferDrawTextBufferView` segfaults when negative layout origins are cast
  to `uint32_t` (repro: x=-1, y=0). Degenerate panes are skipped, zoom
  siblings are placed truly off-screen, and text-buffer / fill draws reject
  negative coordinates. Fatal TUI logs now append a best-effort backtrace.
- **Conversation switch/delete cancel wait (#46).** After confirming
  “switch anyway?” (or deleting a conversation with a turn in flight), the
  main thread no longer spins in a blind `sleep_for` loop. Cancel is deferred
  onto the REPL loop so input keeps running: a `cancelling… (Esc to abort)`
  spinner paints via the output pump, Esc / Ctrl-C abandons the pending op
  (queued follow-ups are preserved until a successful switch/delete), and the
  turn is cancelled through a per-request `CancelToken` so sibling panes keep
  streaming. Kitty CSI-u Esc/Ctrl-C is recognized the same way as the line
  editor. Esc during a normal turn also uses the pane token when present.
- **`/search` wiring.** Operator-typed `/search` in the TUI now mirrors
  `/fetch` (bypasses the focused agent's capability gate and injects
  results into the turn). `arbiter --send` wires the same search/MCP
  tools as the TUI. Brave error responses surface `detail`/`code`
  (including HTTP 422 invalid tokens), and responses are requested with
  libcurl auto-decompression per Brave's documented client headers.
  Research starter capabilities now list `/search` and `/browse`
  explicitly.
- **`chat_command_tui` sidebar rename flake.** Seed the conversation via
  `/chat title` (no in-flight agent turn) and poll for the renamed title
  so macos-arm64 CI no longer races sidebar focus against a dummy API
  request.

## [0.7.3] — 2026-07-09

Adds TUI search and command-discovery surfaces, and fixes ctrl-key
bindings and Esc going dead under terminals that speak the kitty
keyboard protocol.

### Added
- **Conversation search.**  `/chat search <term>` and a live type-to-filter
  (`/` in the history sidebar) find matching conversations by title or
  content.
- **In-pane scrollback search.**  `/find <term>` searches the focused
  pane's scroll buffer and jumps between matches.
- **Command palette.**  Ctrl-P opens a fuzzy-matched palette covering
  every `/`-command, ranked by prefix/substring/description/subsequence
  match quality.
- **Reverse history search.**  Ctrl-R starts a readline-style
  reverse-incremental search over input history, now shared live across
  panes.

### Fixed
- **Ctrl-key bindings and Esc dead under the kitty keyboard protocol.**
  Terminals that speak the protocol (kitty, Ghostty, WezTerm, foot)
  re-encode ctrl+letter and Esc as `CSI ... u` escape sequences instead
  of legacy control bytes once OpenTUI's capability handshake opts in.
  Rather than only trying to suppress the terminal's use of the
  protocol — inherently racy, since the terminal's capability reply is
  asynchronous relative to arbiter's setup window — the input layer now
  decodes these reports directly back into the legacy bytes its
  dispatch already understands. Also handles the alternate-key and
  event-type colon subfields real terminals send once "report alternate
  keys" is active, which a first pass at the decoder missed, silently
  dropping every real-world report.

## [0.7.2] — 2026-07-08

Patch release: a thread-safety and performance sweep across the loop
manager, theme system, provider circuit breaker, and API server.  No
functional surface changes — every fix hardens behavior that already
existed.

### Added
- **API server connection cap.**  The thread-per-connection accept loop
  is now bounded (default 256; override with `ARBITER_MAX_CONNECTIONS`).
  Connections past the cap get `503 Service Unavailable` +
  `Retry-After: 1` instead of an unbounded detached thread each.

### Fixed
- **LoopManager data races.**  Loop entry state (`output_log`, `state`,
  `iter`, `stop_reason`) was written by the loop thread under one mutex
  (or none) and read by `/log`, `/loops`, and `/watch` under another —
  reading the log vector mid-append was undefined behavior.  All mutable
  entry fields are now guarded by the per-entry mutex on both sides, and
  `kill()` / `reap_stopped()` / the destructor detach entries from the
  registry before joining so two concurrent `/kill`s of the same loop
  can't use-after-free.
- **Theme switch race.**  `/theme` rebuilt a shared `Theme` and mutated
  the global `TuiDesign` in place while loop threads, pane exec threads,
  and the output pump were reading them.  The active design is now
  published as an immutable snapshot (lock-free reads; snapshots retained
  so handed-out references never dangle) and `theme()` caches per-thread.
- **Circuit breaker stuck half-open.**  A probe admitted in `HalfOpen`
  that ended without a verdict — connection failure in `complete()`'s
  early-return path, or a user cancel — leaked `probe_in_flight` and
  rejected the provider with `circuit_open` until process restart.
  Connect failures now record a failure; cancelled probes resolve via a
  new `record_abandoned()` that reopens the breaker without counting
  toward the Closed-state failure threshold.
- **`ApiClient::complete()` ignored `cancel()`.**  A cancel during a
  blocking call shut the socket down, then the retry loop reconnected
  and retried up to 4 times (with backoff sleeps) anyway.  `complete()`
  now checks the cancel flag at every attempt boundary and aborts.
- **Agent stats race.**  Token/request counters are now atomic;
  `status_summary()` and the autosave serializer read them while a turn
  is mid-flight.
- **Idempotency cache unbounded growth.**  `prune_expired()` was never
  called in production, and clients mint a fresh `Idempotency-Key` per
  request, so expired entries accumulated for the life of the server.
  `put()` now runs the TTL sweep amortized every 512 inserts.
- **Missing `<array>` include** in `span_scroll_append.h` broke the
  build on toolchains that don't provide it transitively.

## [0.7.1] — 2026-07-06

Patch release: two concurrency fixes in the API client and the
conversation-titling worker.

### Fixed
- **Per-provider connection pools.**  The process-wide `ApiClient`
  serialized every upstream call behind one global connection mutex held
  for the entire request — panes advertised as independently streaming
  couldn't overlap, even across different providers (#48).  Each call now
  leases a distinct connection from a small per-provider pool and does
  all socket I/O with no shared lock held; `cancel()` semantics are
  unchanged.  Adds a deterministic barrier-based regression test
  (`unit_api_client_pool`).
- **Title worker use-after-free.**  The detached conversation-titling
  worker captured a raw `Orchestrator*` and could dereference it after
  the orchestrator was destroyed if the title call outlived its 10s
  timeout during shutdown (#51).  The worker now mints and owns a
  standalone side client (`Orchestrator::make_side_client()`) and never
  touches orchestrator-owned state; timed-out calls are cancelled so the
  worker exits promptly instead of lingering on a hung connection.

## [0.7.0] — 2026-07-06

Minor release focused on durable conversation history in the TUI.  Sessions
autosave after every turn, appear in a leading history sidebar with
model-refined titles, and replay their transcript when you switch back.
The `/chat` command family covers list/new/switch/title/delete/purge from
the REPL.

### Added
- **Per-turn autosave.**  ConversationStore persists each completed turn on a
  background save thread with a one-deep "latest wins" queue; `flush()` drains
  on exit.
- **Conversation titling.**  Deterministic titles land instantly; an async
  model call refines them once per conversation.  Manual `/chat title` locks
  the title against further auto-titling.
- **Transcript replay.**  Switching conversations in the TUI replays the
  saved transcript into the scroll region instead of starting blank.
- **History sidebar UX.**  Entry layout, inline rename/delete confirm, page
  up/down, and keyboard navigation (`Ctrl-w h` toggle, `Ctrl-w H` focus).
- **`/chat` command family.**  `list`, `new`, `switch`, `title`, `delete`,
  and `purge` for conversation management from the REPL.

### Changed
- **Sidebar separators.**  History and stats sidebars use dithered shade glyphs
  (▏/▕) instead of thin vertical rules so pane seams don't show gaps.

### Fixed
- **ConversationStore.**  Deadlock fix, atomic session-file writes,
  id-pinned selection across reloads, and soft delete (filter from list,
  keep file until purge).

## [0.6.0] — 2026-07-05

Minor release after the 0.5.0 beta line.  OpenTUI is now the sole TUI
engine; the session sidebar tracks context, agent, todos, and schedules;
hosted models route through OpenRouter; and the interactive REPL has
full tool parity with the HTTP API (`/search`, `/todo`, `/schedule`,
structured `/mem`, MCP, A2A, artifacts).  Streaming output renders
unified-diff blocks inline; the pane chrome is stripped back to
scrollback + an accent-styled input strip.

### Added
- **TUI/API command parity.**  The REPL wires the same tenant-scoped tool
  invokers as `/v1/orchestrate` — web search, todos, schedules, structured
  memory, MCP, A2A, exec, and conversation artifacts — and runs the
  background scheduler while the TUI is open.
- **Session sidebar.**  Context fill %, agent/model, task title, todos,
  schedules, loops, and cost — toggled with `Ctrl-w s` on wide terminals.
- **OpenRouter routing.**  Hosted model ids resolve through OpenRouter
  instead of per-provider keys where configured.
- **Inline diff rendering.**  Agent replies that emit ` ```diff ` fences
  render as styled before/after blocks in the scroll region.

### Changed
- **OpenTUI cutover.**  Legacy TUI backend removed; design tokens live in
  `~/.arbiter/tui_design.json`.
- **TUI layout.**  Pane header chrome removed; user input uses the header
  palette (dark background + orange accent strip).
- **Constitution.**  Agent configs now require ` ```diff ` fences when
  proposing code edits so the TUI can render patches.

## [0.5.0-beta2] — 2026-05-20

Second **beta** in the 0.5.0 line.  Focus is the agent-facing todo
tracker — wiring it into the constitution and the master-depth turn
so the agent actually reaches for it, plus filling out the HTTP
surface so external clients can drive the same store without N+1
round-trips.  Also a documentation cleanup: the unreleased hosted
preview is gone from the public docs while it's still in
development, and a SwiftUI iOS reference client (Newton) is linked
from the README as a worked example of consuming the HTTP+SSE API.

### Added
- **Todos surfaced to the master agent.**  A new `todos` constitution
  bundle teaches `/todo add | list | start | done | cancel | describe
  | subject | delete` with rules on when to mark progress vs. when not
  to re-list, and is on by default for `index`.  Every master-depth
  turn now receives an `[OPEN TODOS] … [END OPEN TODOS]` preamble
  prepended to the user message (same lifecycle as the lesson probe),
  so the agent walks into each turn already aware of in-flight work
  instead of having to remember to call `/todo list` itself.  Symmetric
  with the sub-agent `[DELEGATION CONTEXT]` envelope, which was already
  carrying open todos for delegated agents.
- **Batch `PATCH /v1/todos`.**  Accepts a JSON array (or `{"todos":[…]}`)
  of `{id, …fields}` objects, applies each independently, returns
  per-row results with `ok` / `errors` totals.  Caps at 500 items per
  batch.  Removes the N round-trip cost of "mark these three done" or
  "sync state from an external tracker" against the HTTP API.  See
  [`docs/api/todos/patch.md#batch-form`](docs/api/todos/patch.md#batch-form).
- **Seed `status` on `POST /v1/todos`.**  Optional `status` field
  accepts `pending` (default) / `in_progress` / `completed` /
  `canceled`.  Terminal seeds stamp `completed_at = created_at` so
  migrated rows don't look like in-flight work that just resolved.
  Useful when backfilling from another tracker.
- **`conversation_id=tenant` (or `unscoped`) filter on `GET /v1/todos`.**
  Returns only `conversation_id = 0` rows — the cross-thread browser
  surface that previously had to either omit `conversation_id` (which
  dumps every thread's rows mixed) or sift through the OR-NULL
  fallback result.
- **`/todo list all` writ.**  Includes terminal (`completed`,
  `canceled`) rows in the renderer for retrospective review.  Bare
  `/todo list` still hides them so the open-work view stays focused.
- **Newton iOS reference client.**  README now links to
  [`tylerreckart/newton`](https://github.com/tylerreckart/newton), a
  SwiftUI app that drives the runtime end-to-end from a mobile
  frontend (bearer auth, streaming `/v1/orchestrate` parsed
  event-by-event, conversation persistence, writ tool-call rendering).
  Starting point for anyone building their own arbiter frontend.

### Changed
- **`/todo list` renderer shows `[p<N>]` position.**  Agents can now
  reason about reorder targets without inferring order from ids.
  Terminal rows (when surfaced via `/todo list all`) get `✓` / `✗`
  markers paired with the existing `▶` for in-progress.
- **Block-form `/todo add` body parser is no longer fooled by
  `/`-prefixed body lines.**  Previously any line starting with `/`
  (file paths, shell commands, URLs in a description) aborted body
  capture; the parser now only bails on recognised writ prefixes.
  When the stream cuts off before `/endtodo`, the runtime soft-commits
  the subject and emits a `WARN: missing /endtodo terminator` instead
  of dropping the create entirely, so the agent's intent isn't lost
  to a network blip; the next turn can `/todo describe <id>: <text>`
  to fill in the body.
- **`TenantStore::create_todo` signature.**  Added an optional 6th
  parameter (`const std::string& status = "pending"`).  Source-compatible
  with existing callers; downstream binaries linking against the
  pre-beta2 ABI need a rebuild.
- **`TenantStore::TodoFilter::conversation_id < 0`** now means
  "unscoped-only" (returns only `conversation_id = 0` rows).  Positive
  retains the OR-NULL fallback, `0` retains "no filter".
- **Version display.**  `INDEX_VERSION` (rendered on the TUI welcome
  card) is now `${PROJECT_VERSION}${ARBITER_VERSION_SUFFIX}`, so
  prerelease tags like `-beta2` surface in the UI without violating
  CMake's strict-numeric `project(... VERSION x.y.z)` parser.

### Removed
- **Hosted-service docs.**  `docs/getting-started/hosted.md` and every
  reference to the managed/SaaS deployment posture have been pulled
  from the documentation.  The hosted product isn't ready for the
  public yet; pointing prospective users at a waitlisted endpoint
  while the local install is the only working path was creating noise
  for no payoff.  Will be reintroduced when the service ships.
- **README "Why arbiter" feature pitch.**  Trimmed to keep the page
  focused on "what it is, how to install, an example session";
  feature exposition lives in the concept docs.

### Fixed
- **Master agent walking into turns blind.**  Open-todo injection
  previously fired only on delegation (`/agent`, `/parallel`); the
  master at depth 0 never saw its own open todos and, paired with the
  missing constitution bundle, never thought to ask.  Net effect was
  a feature that essentially did not exist for the master agent
  through the API.  See the bundle + injection items in **Added**.



This is a **beta** release.  The feature surface is operational
hardening — none of it changes existing agent or HTTP semantics — but
the per-tenant Docker sandbox is a substantial new module without
automated test coverage in v1 (verified only via the
`examples/sandbox/setup.sh --check` smoke test).  Treat sandbox
deployments accordingly; the rest of the surface (idempotency,
metrics, circuit breaker, structured logging) is exercised by the
test suite and safe to depend on.

### Added
- **Per-tenant Docker sandbox for `/exec`.**  Opt-in via
  `ARBITER_SANDBOX_IMAGE=<image>`.  One persistent container per
  tenant, started lazily on the first `/exec`, with a bind-mounted
  `/workspace` directory shared by `/exec`, `/write`, and `/read`.
  Containers run `--network=none --read-only --tmpfs /tmp:rw,size=64m`
  with configurable memory / CPU / pids caps and a per-exec wall-clock
  kill (`ARBITER_SANDBOX_MEMORY_MB`, `_CPUS`, `_PIDS_LIMIT`,
  `_EXEC_TIMEOUT`; defaults 512m / 1.0 / 256 / 30s).  Workspace bytes
  persist across requests and server restarts at
  `~/.arbiter/workspaces/t<tid>/` (mode `0700`); a soft per-tenant
  quota (`ARBITER_SANDBOX_WORKSPACE_MAX_BYTES`, default 1 GiB) is
  enforced at `/write` time.  A background reaper stops containers
  idle past `ARBITER_SANDBOX_IDLE_SECONDS` (default 30 min) without
  touching the workspace; the next op cold-starts a fresh container.
  Survivor containers from a prior process are probed with
  `docker exec true` and re-attached when responsive, force-removed
  and rebuilt when not.  `stop_all()` runs on SIGTERM as part of the
  drain sequence.  Failure-mode philosophy is **degrade, don't crash**:
  a misconfigured or unreachable sandbox leaves `/exec` returning the
  standard `ERR:` block and the server runs unaffected.  An example
  Debian-slim image and `setup.sh` (with `--check`, `--teardown`,
  `--print-only` modes) ship in `examples/sandbox/`.
  See [`docs/concepts/sandbox.md`](docs/concepts/sandbox.md).
- **Idempotent retries on write-creating POSTs.**  `Idempotency-Key`
  header on `/v1/orchestrate`, `/v1/conversations/:id/messages`, and
  `/v1/agents/:id/chat`.  The runtime records `(tenant_id, key) →
  request_id` and treats any subsequent request with the same key as a
  join-or-replay of the original: still-running keys live-tail the
  original SSE from its current position, completed keys replay the
  durable event log from `seq=0` to terminal `done`, deleted keys
  return `404` instead of silently rerunning.  Keys are tenant-scoped,
  opaque (≤ 256 chars), and the cache is in-memory with a 24h TTL —
  durable dedup across server restarts is a Phase-3 follow-up.  CORS
  allow-list extended to include `Idempotency-Key` and `If-None-Match`.
  See [`docs/api/orchestrate.md#idempotency`](docs/api/orchestrate.md#idempotency).
- **`GET /v1/metrics` (Prometheus exposition format).**  Unauthenticated
  scrape endpoint — restrict at the reverse proxy.  Counters and
  gauges cover request flow (`arbiter_requests_started_total`,
  `_completed_total`, `_duration_ms_sum`, `arbiter_in_flight`; labels
  `tenant`, `route`, `ok`), provider call health (`_calls_total`,
  `_retries_total`, `_5xx_total`, `_429_total`,
  `_circuit_open_total`; label `provider`), sandbox container
  lifecycle (`_exec_total`, `_exec_timeout_total`,
  `_container_started_total`, `_reaped_total`, `_rebuilt_total`,
  `_containers_running`), idempotency hit/miss
  (`_replay_total`, `_miss_total`), and rate-limit rejections
  (`arbiter_rate_limited_total{reason}`).  Every registered metric
  emits its `HELP` + `TYPE` headers on a fresh-start scrape so
  dashboards don't NaN out.  See
  [`docs/api/metrics.md`](docs/api/metrics.md).
- **Per-provider circuit breaker.**  Sits in front of the per-request
  retry loop.  After 5 consecutive failures (5xx or 429 past the retry
  budget) against the same provider, the breaker opens for a 30 s
  cooldown.  Calls while open fast-fail with a structured `error_code:
  "circuit_open"` on the `done` SSE event instead of every parallel
  request burning four retries against a clearly-unhealthy upstream —
  typically tens of milliseconds vs 7+ seconds.  The cooldown elapses
  into a half-open probe; success closes, failure reopens with a fresh
  cooldown.  Defaults are tuned conservatively for v1; operator-tunable
  thresholds are a Phase-5 follow-up.  See
  [`docs/concepts/operations.md#provider-circuit-breaker`](docs/concepts/operations.md#provider-circuit-breaker).
- **Structured operational logger.**  Startup, recovery sweep,
  shutdown drain, sandbox enable/disable, idle reaping, and circuit
  breaker transitions all route through `Logger::global()` and emit
  either human-readable (`[HH:MM:SS] [level] event key=value`) or JSON
  (`{"ts":"…","level":"…","event":"…",…}`) lines on stderr.  Switch
  with `ARBITER_LOG_FORMAT=json|human` (default `human`).  Per-request
  `--verbose` SSE mirroring keeps the existing human format; only the
  operational-event stream is structured in v1.
- **`GET /v1/admin/audit`.**  Append-only log of every mutation through
  `/v1/admin/*` (create / update / disable tenant).  Each row records
  actor, action, target, and JSON snapshots of state before and after.
  Reverse-chronological with `before_id` / `limit` cursor pagination
  (default 50, hard cap 200).  The runtime never edits or deletes audit
  rows — retention is the operator's policy decision.  Backed by a new
  `admin_audit` table on `tenants.db`.  See
  [`docs/api/admin/audit.md`](docs/api/admin/audit.md).

### Changed
- API server startup banner re-renders the sandbox status line (image,
  network, caps, exec timeout) after the screen clear so operators
  don't have to scroll up to find it.  The scrollback erase
  (`\033[3J`) was dropped from the banner sequence so pre-clear ctor
  logs (recovery sweep, sandbox usability failures) remain available
  for forensics.
- `examples/sandbox/setup.sh` gained `--check` (smoke-test an existing
  image with the same flags `/exec` uses at runtime), `--teardown`
  (stop containers + remove image; workspace bytes left in place), and
  a `--yes` confirmation skip.  The smoke test catches "image built
  but won't run under `--read-only`", "tmpfs mount rejected", "no
  `/bin/sh` in the image" — failures that would otherwise only surface
  inside `/exec`.

### Known limitations
- **Sandbox has no automated test coverage in this release.**  Smoke
  testing is via `examples/sandbox/setup.sh --check`.  A test suite
  exercising container lifecycle, quota enforcement, survivor re-attach,
  and reaper behavior is targeted for the 0.5.x point releases.
- **Circuit breaker thresholds are hard-coded.**  Env-var tunables
  (`ARBITER_CIRCUIT_*`) are a Phase-5 follow-up.

## [0.4.5] — 2026-05-11

### Added
- **Durable in-flight execution.**  Every `/v1/orchestrate` (and
  conversation message, agent chat, A2A dispatch) call now mirrors
  its SSE event stream into two new tables on `TenantStore`:
  `request_status` (one row per run; state, agent, timestamps,
  last_seq) and `request_events` (append-only log indexed
  `(request_id, seq)`).  `text` deltas coalesce into ~2 KiB chunks
  before persistence; other events persist 1:1.
  - **`GET /v1/requests/:id/events?since_seq=N`** replays the
    persisted backlog as SSE frames, then live-tails via a per-
    request in-process bus (`RequestEventBus`) until the run hits
    `done`.  Each frame carries the seq as the SSE `id:` field so
    re-reconnects need not parse payloads.
  - **`GET /v1/requests`** + **`GET /v1/requests/:id`** expose the
    run-level metadata for listing / discovery.
  - **A2A `tasks/resubscribe`** translates each persisted event into
    the appropriate `TaskStatusUpdateEvent` / `TaskArtifactUpdateEvent`
    envelope, replacing the prior `UnsupportedOperation` rejection.
    Backed by the same store + bus.
  - **Recovery sweep** at `ApiServer::start()` marks every
    `state='running'` row from a previous process as `failed` so
    reconnecting clients see a clean terminal signal.
  See [`docs/concepts/durable-execution.md`](docs/concepts/durable-execution.md).
- **Self-reflection / learned-from-failure.**  New `lessons` table on
  `TenantStore`, agent-scoped (`tenant_id`, `agent_id`).  Three
  integrated mechanisms:
  - **`/lesson` writ** with `<signature>: <text>` single-line and
    `/endlesson`-terminated block forms; subcommands `list`, `search
    <query>`, `delete <id>`.  Backed by a `LessonInvoker` callback
    threaded through `Orchestrator` and `execute_agent_commands`.
  - **Intra-turn loop detection.**  The dispatch loop tracks
    `(tool, args)` signatures that produced `ERR:`; when the same
    signature ERRs twice in a row a `[LOOP DETECTED]` block is
    prepended to the next user-role tool-result block, naming the
    offending call so the agent breaks out instead of grinding.
  - **Pre-turn lesson injection.**  At the top of each top-level
    `run_dispatch`, the runtime probes the agent's lessons against
    the user's prompt (substring match on signature + lesson_text),
    bumps `hit_count` on surfaced rows, and prepends a `KNOWN
    PITFALLS` block before the message.
  HTTP surface: `POST/GET /v1/lessons`, `GET/PATCH/DELETE
  /v1/lessons/:id`.  See
  [`docs/concepts/lessons.md`](docs/concepts/lessons.md).
- **Memory consolidation + age decay.**  `/mem add entry --supersedes
  #N,#M` (and `POST /v1/memory/entries` `supersedes_ids: [N, M]`)
  creates a synthesis entry that supersedes the listed prior entries
  in one transaction: a `supersedes` relation lands per pair, the
  prior entries are invalidated (`valid_to=now()`).  Manual
  supersession overrides the existing advisor-driven auto-supersede
  pass.  Also: BM25 search now multiplies scores by a piecewise
  recency factor when `MemoryConfig.age_decay` is on (default on; 90d
  half-life, 0.5 floor) — old entries rank lower without
  disappearing.  HTTP path opt-in via `decay=true` query param.
- **Per-tenant rate / concurrency limiter.**  Bounded in-flight LLM
  requests per tenant (`ARBITER_TENANT_MAX_CONCURRENT`) plus a
  token-bucket rate limit (`ARBITER_TENANT_RATE_PER_MIN`,
  `ARBITER_TENANT_RATE_BURST`); both default to 0 = unlimited.
  Surplus requests on the expensive routes (`/v1/orchestrate`,
  conversation messages, agent chat, A2A dispatch) get `429 Too Many
  Requests` with `Retry-After`.  Cheap reads unaffected.  See
  [`docs/concepts/operations.md`](docs/concepts/operations.md#per-tenant-rate--concurrency-limiting).
- **Agent-facing todo tracker.** New `todos` table on `TenantStore` plus
  `/todo` writ with `add` (single-line and `/endtodo` block forms),
  `list`, `start`, `done`, `cancel`, `delete`, `describe <id>: <text>`,
  and `subject <id>: <text>` subcommands.  Conversation-scoped by
  default with tenant-wide as the unscoped fallback (same OR-NULL
  visibility structured memory uses).  Pipeline-memory injection
  surfaces a calling conversation's open todos to delegated sub-agents
  (both `/agent` and `/parallel`) inside the `[DELEGATION CONTEXT]`
  envelope so they can mark progress without re-discovering the list.
  HTTP surface: `POST/GET /v1/todos`, `GET/PATCH/DELETE /v1/todos/:id`.
  See [`docs/concepts/todos.md`](docs/concepts/todos.md).
- **Background scheduler.** New `/schedule "<phrase>": <message>` writ that
  defers or recurs agent work; the API server's tick thread fires due
  tasks through the same orchestrator path that `/v1/orchestrate` uses,
  persists the result as a `task_runs` row, and publishes a notification
  on a long-lived SSE stream.  Strict NL parser covers `in N (min/h/day/
  week)`, `at HH:MM`, `tomorrow [at HH:MM]`, `on YYYY-MM-DD [at HH:MM]`,
  `every (hour|hourly)`, `every N (min|hour)s`, `every (day|daily) [at
  HH:MM]`, `every (week|weekly|<weekday>) [at HH:MM]`.  HTTP surface:
  `POST/GET /v1/schedules`, `GET/PATCH/DELETE /v1/schedules/:id`,
  `GET /v1/schedules/:id/runs`, `GET /v1/runs[?since=&task_id=]`,
  `GET /v1/runs/:id`, `GET /v1/notifications/stream`.  See
  [`docs/concepts/scheduler.md`](docs/concepts/scheduler.md).

## [0.4.4] — 2026-05-07

### Added
- **Vision input.** `Message::content` extends to a parts array
  (`ContentPart` — `TEXT` or `IMAGE`); body builders for all four
  providers emit each provider's native multipart shape (Anthropic
  content blocks, OpenAI `image_url` parts, Gemini `inlineData` /
  `fileData`). `POST /v1/orchestrate` accepts `message` as either a
  string (legacy) or an array of parts; URL-form image references are
  fetched server-side with a 20 MB cap and `image/*` content-type
  validation. Tool results carry images: `/fetch` on an image
  Content-Type and `/read` on an image artifact attach the bytes to the
  next turn as an image part instead of a textified body, so vision-
  capable agents can act on images they retrieve. `Agent::send` and
  `Orchestrator::send_streaming` gain parts overloads; the legacy string
  versions wrap a single text part. See
  [`docs/concepts/writ.md`](docs/concepts/writ.md#image-content-in-tool-results)
  and [`docs/api/orchestrate.md`](docs/api/orchestrate.md#vision-input).
- **Google Gemini provider.** Models prefixed `gemini/<id>` route to
  Google's `generativelanguage.googleapis.com` endpoint
  (`/v1beta/models/<id>:streamGenerateContent` for streaming,
  `:generateContent` otherwise). Authentication via `x-goog-api-key`
  header. Key discovery follows the existing pattern: `GEMINI_API_KEY`
  env var, falling back to `~/.arbiter/gemini_api_key`. Initial catalog
  in `/v1/models` includes `gemini-2.5-pro`, `gemini-2.5-flash`,
  `gemini-2.5-flash-lite`, and `gemini-2.0-flash`. Translates the
  codebase's `assistant` role to Gemini's `model`, hoists the system
  prompt into `systemInstruction`, and surfaces `cachedContentTokenCount`
  on `cache_read_tokens` so the billing service can discount implicit
  context-cache hits the same way it does for Anthropic / OpenAI.
  `RESOURCE_EXHAUSTED` and `UNAVAILABLE` are treated as retryable.

## [0.4.3] — 2026-05-07

### Added
- **Agent2Agent (A2A) v1.0 protocol — both directions.** Tenant agents are
  reachable as A2A endpoints at `POST /v1/a2a/agents/:id`
  (`message/send`, `message/stream`, `tasks/get`, `tasks/cancel`);
  per-agent `AgentCard`s served at
  `GET /v1/a2a/agents/:id/agent-card.json` with an unauth discovery stub
  at `/.well-known/agent-card.json`. Outbound: arbiter agents call remote
  A2A agents listed in `~/.arbiter/a2a_agents.json` via a new
  `/a2a list|card|call` slash command, surfaced to the master orchestrator
  alongside the local agent roster. Tasks persist in a new `a2a_tasks`
  table; cancel reuses the in-flight registry so `tasks/cancel` and
  `POST /v1/requests/:id/cancel` resolve through the same handle. v1.0
  only; `tasks/resubscribe` and push notifications deferred. See
  [`docs/concepts/a2a.md`](docs/concepts/a2a.md).
- `public_base_url` server option for TLS-fronted deploys; falls back to
  the `Host` header otherwise.
- **Example MCP server registry** at `examples/mcp_servers.json` covering
  GitHub, Sentry, Linear, and Slack via the `mcp-remote` stdio↔HTTP
  bridge. Engineering starter agents (`backend`, `devops`, `frontend`,
  `reviewer`, `planner`, `research`) now declare `/mcp` in their
  capabilities and carry per-agent rules naming which servers to call
  for which work.
- **Writ — the slash-command DSL is now a named concept.** New
  [`docs/concepts/writ.md`](docs/concepts/writ.md) defines the language
  agents emit inline (verbs, block forms, agent-as-first-class-value,
  per-agent dialects via the capability allowlist). README, philosophy
  doc, and concept index reference it by name.
- **Getting-started documentation** at
  [`docs/getting-started/`](docs/getting-started/index.md) with two
  paths: `hosted.md` (managed endpoint, limited-preview waitlist) and
  `local.md` (install + first run). Index page leads with the hosted
  option for evaluators; local for self-hosters who want `/exec` and
  filesystem access.
- README rewrite: new "Why arbiter" section surfacing the four
  differentiators (writ vs. JSON tool-use, multi-agent composition as a
  language primitive, structural advisor gating, single binary /
  local-first); a worked example session showing writs in flight; a
  hosted-preview pointer in the lead. Install/Setup/Running collapsed
  into a single Quick start block that defers to getting-started.

### Changed
- Tool callbacks (memory scratchpad, structured memory, MCP, search,
  artifacts) factored into shared factories so `/v1/orchestrate` and the
  new A2A handlers install identical behaviour from one source.
- **`--api` verbose log overhauled.** Replaces the prior
  `POST /orchestrate … DONE` one-liner with a two-form layout: marker
  events (`request_received`, `stream_start`) on dedicated lines, inline
  events (`tool_call`, `advisor`, `file`, `done`, `error`) as
  `event: <name> · <value>`. Streamed text and thinking deltas are
  suppressed (they already mirror over SSE; duplicating multi-thousand-
  token prose drowned out the event spine). Successful `stream_end`
  stays quiet so parallel fan-outs don't flood; failures still surface.
  Token totals on `done` switch to a wall-clock seconds + USD-cost
  format when pricing is available, falling back to in/out token
  counts otherwise.
- **Concept docs moved out of `docs/api/`.** All twelve files relocated
  from `docs/api/concepts/*` to `docs/concepts/*` so concepts are
  reachable from CLI / TUI / getting-started without crossing into the
  HTTP API tree. Inbound links across `docs/`, the README, and
  `docs/philosophy.md` updated. External bookmarks pointing at the old
  paths will 404 — there is no redirect layer in the markdown.
- Documentation expanded for the A2A surface: new `docs/api/a2a/`
  endpoint pages (`well-known.md`, `agent-card.md`, `dispatch.md`),
  `docs/concepts/a2a.md` concept doc, and `docs/cli/a2a-agents.md` for
  the local registry + slash command. `docs/api/concepts/sse-events.md`
  documents the new A2A-aware event shapes.

## [0.4.2] — 2026-05-06

### Added
- **Memory retrieval overhaul.** FTS scoring now reciprocal-rank-fuses
  conversation-scoped and tenant-wide passes (conv. weight 1.5, tenant
  1.0, k=60), adds a `NEAR(…, 8)` clause for 2–6-token queries, and
  threads type / authored-date / supersession into the rerank prompt and
  `/mem entries` output.
- Advisor-driven enrichment on `/v1/memory/entries`: opt-in query
  expansion (`?expand=<model>`), auto-tagging (`auto_tag=<model>`), and
  auto-supersession (`supersede=<model>`). All benign on failure — search
  and writes proceed if the advisor is unreachable or returns garbage.
- Question-intent routing: regex classifier maps cue words to entry-type
  boosts (1.3× BM25). Zero LLM cost. Default on; disable via
  `?intent=off` (HTTP) or `memory.intent_routing=false` (agent).
- `created_at` override on entry create — backfills historical transcripts
  with their real authored timestamps so temporal queries land at the
  right point in time.
- Per-agent `MemoryConfig` block on the constitution
  (`intent_routing` / `search_expand` / `auto_tag` / `auto_supersede`).
  The four shipped agents with advisors (`backend`, `devops`, `frontend`,
  `research`) opt in to all three advisor-driven toggles by default.

### Changed
- `StructuredMemoryWriter` callback gained a `caller_id` parameter so the
  HTTP writer can read the caller's Constitution and decide whether
  `auto_tag` / `auto_supersede` fire on the write.
- LongMemEval bench: per-session `haystack_dates` ingested as real
  timestamps; rerank top-k default 5 → 10; query / grade pipelines now
  surface authored dates and conversation ids.

## [0.4.1] — 2026-05-03

### Added
- **Advisor SSE event surface.** New `escalation` event signals
  out-of-band advisor halts; new `advisor` event reports every gate
  decision (`consult` / `gate_continue` / `gate_redirect` / `gate_halt` /
  `gate_budget`) with the executor's terminating-turn preview so a
  consumer can diagnose redirects without spelunking the transcript.
  Orchestrator hooks: `set_escalation_callback`,
  `set_advisor_event_callback`.

### Changed
- Tighter executor↔advisor handshake: redirect-budget plumbing,
  malformed-signal handling (`advisor.malformed_halts` defaults closed),
  consistent terminating-turn previews on every gate event.
- Starter agents (`agents/*.json`) are now embedded into the binary at
  build time via `cmake/embed_starters.cmake` instead of duplicated in
  C++ source — single source of truth across `arbiter --init` and the
  first-run wizard.

## [0.4.0] — 2026-04-30

### Added
- External billing-service integration. When `ARBITER_BILLING_URL` is
  set, every authenticated request is exchanged for a workspace_id via
  `POST /v1/runtime/auth/validate`, pre-flighted against
  `POST /v1/runtime/quota/check`, and post-turn telemetry is fired
  (fire-and-forget, idempotent on `request_id-tN` per turn) to
  `POST /v1/runtime/usage/record`. With the env var unset, the runtime
  is a thin pass-through using the operator-supplied provider keys —
  no eligibility checks, no caps. The runtime ships no billing-service
  reference implementation; commercial deployments must implement the
  protocol against a service of their choosing.
- Per-tenant artifact store. `POST /v1/conversations/:id/artifacts` and
  the matching list / get / raw / delete endpoints persist agent-
  generated files server-side with per-conversation and per-tenant
  quotas.
- Structured-memory graph: `/v1/memory/entries`, `/v1/memory/relations`,
  and `/v1/memory/graph` for typed nodes + directed labeled edges.
- **FTS5 + Okapi-BM25 ranked search** for `/v1/memory/entries?q=…` and
  the agent-side `/mem search`. Replaces the previous `LIKE %q%`
  substring scan. Per-field weights (title ×10, tags ×8, content ×4,
  source ×2) shipped as defaults; rebuild-guarded by
  `PRAGMA user_version` so existing tenants migrate on first open.
- **Metadata-as-boost ranking**: when `q` is set, type and tag filters
  no longer hard-`WHERE` away non-matching rows; they multiply the
  BM25 score (type ×1.3, tag ×1.2). Filters still apply as hard
  predicates when `q` is omitted.
- **Temporal validity columns** `valid_from` / `valid_to` on memory
  entries. New `POST /v1/memory/entries/:id/invalidate` and matching
  `/mem invalidate <id>` slash command. `EntryFilter::as_of` returns
  the historical view at a timestamp using half-open
  `[valid_from, valid_to)` windows. `delete_entry` is unchanged
  (still hard-delete); soft-deletion is the dedicated invalidate path.
- **Conversation-scoped graduated search**. New `conversation_id`
  column on entries plus `search_entries_graduated()`: a
  conversation-scoped first pass, then a tenant-wide fill if results
  are sparse. Exposed as
  `?conversation_id=<id>&graduated=true` on the entries endpoint and
  the default scope hint for agent-side `/mem search`.
- **Optional LLM reranker** via `?rerank=<model>` on the HTTP entries
  endpoint and `/mem search --rerank=<model>` on the agent path. Both
  paths share `rerank_with_advisor()`; the agent path billed through
  the existing orchestrator advisor invoker, the HTTP path through a
  per-request `ApiClient` keyed off the operator's provider keys.
- **LongMemEval benchmark harness** at `bench/longmemeval/`
  (Python-stdlib, ingest + query). Headline numbers on
  `longmemeval_s` at v0.4.0: bm25 R@5 = 34.8%, graduated R@5 = 80.6%,
  graduated + Haiku rerank R@5 = 85.2%. See `README.md` for the full
  table and comparison to other systems.
- Tenant-stored agent catalog: `POST /v1/agents` and friends let
  callers register agent definitions once and reference them by id on
  subsequent `/v1/orchestrate` and `/v1/conversations/:id/messages`
  calls without re-sending the full constitution.
- HTTP streaming via SSE for `/v1/orchestrate` and the chat / messages
  endpoints, with `text`, `tool_call`, `file`, `token_usage`,
  `sub_agent_response`, and `stream_end` events per turn.
- Multi-pane terminal client. `/parallel` fan-outs render in their own
  panes; pane chord (`Ctrl-W`) is the entry point for split / focus
  operations.
- Playwright-MCP integration for `/fetch` against JS-heavy pages.
- Tenant authentication via SHA-256 hashed bearer tokens; admin
  endpoints gated by a separate admin token.
- Apache 2.0 LICENSE.
- `SECURITY.md` with private vulnerability-reporting path,
  in-scope / out-of-scope policy, and operator-hardening notes.
- `CONTRIBUTING.md`.
- CI workflow that builds and runs `ctest` on every PR + push to main
  across macOS arm64, macOS x86_64, and Ubuntu 22.04.

### Changed
- **Breaking**: `tenant_store` no longer carries billing fields.
  `Tenant.monthly_cap_uc`, `month_yyyymm`, and `month_to_date_uc` are
  gone; `ConversationMessage.billed_uc` is gone; the `usage_log` table
  is dropped on first open of an upgraded DB; `record_usage`,
  `set_cap`, `list_usage`, `usage_summary`, `UsageEntry`, `UsageBucket`,
  and `CostParts` are removed from the public API.
- **Breaking**: `/v1/admin/usage` and `/v1/admin/usage/summary` removed
  — the usage ledger lives in the external billing service.
- **Breaking**: SSE event shapes lost their cost fields.
  `token_usage` no longer carries `provider_micro_cents`,
  `billed_micro_cents`, `markup_micro_cents`, or `mtd_micro_cents`.
  `done` no longer carries `cap_exceeded`, `provider_micro_cents`,
  `billed_micro_cents`, or `markup_micro_cents`. `error` events for
  billing-service denials carry `reason`, `*_micro_cents` budget
  fields, and a human-readable `message` instead.
- **Breaking**: `POST /v1/admin/tenants` no longer accepts `cap_usd` or
  `monthly_cap_micro_cents` in the body. `PATCH /v1/admin/tenants/:id`
  only accepts `disabled`.
- **Breaking**: CLI `--add-tenant` no longer takes `--cap`, and
  `--tenant-usage` is gone.
- **Breaking**: `/v1/models` no longer includes pricing fields. The
  endpoint returns `id` + `provider` only; pricing now lives in the
  billing service's rate card.
- `release.yml` now publishes to this repo's own GitHub Releases via
  `GITHUB_TOKEN`; the previous public-companion-repo flow and
  `RELEASES_REPO_TOKEN` requirement are gone.
- `bump-homebrew.yml` updated to bump the `arbiter` formula (was
  pointing at the legacy `index` formula and download URL).
- Per-agent palette in API-mode logs swapped to a 256-colour muted
  scheme so siblings in `/parallel` fan-outs stay distinguishable
  side-by-side.
- Startup banner replaced with new ASCII art.

### Removed
- `cost_tracker` module deleted entirely. Local pricing tables and the
  REPL session-cost footer are gone — pricing is now external.
- `markup_uc`, `usd_to_uc`, `uc_to_usd`, `uc_to_cents_ceil` helpers
  removed.
- `is_priced(model)` removed from `api_client.h`.
- Welcome-card / chrome integration tests in `tests/test_tui.cpp`
  deleted. Tests focus on real input-handling behavior, not visual
  polish.
- Hardcoded `/Users/tyler/dev/index/build/index` fallback paths in
  test files removed; tests now refuse to build if
  `INDEX_TEST_BINARY` isn't set by CMake.

### Fixed
- `tui_integration` test target renamed to `line_editor` and pruned to
  the 9 input-handling test cases that actually pass deterministically.
- Removed the dead `Ctrl-W kills word` test (the REPL's chord handler
  consumes `Ctrl-W` for pane splits, so the kill-word path it asserted
  is unreachable in the running binary).

## [0.3.6] and earlier

Pre-changelog. See `git log` for history.
