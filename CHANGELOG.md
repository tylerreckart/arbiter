# Changelog

All notable changes to arbiter are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
loosely while pre-1.0 (breaking changes can land on minor bumps).

## [Unreleased]

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
