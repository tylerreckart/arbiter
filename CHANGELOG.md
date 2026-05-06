# Changelog

All notable changes to arbiter are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
loosely while pre-1.0 (breaking changes can land on minor bumps).

## [Unreleased]

### Added
- **Agent2Agent (A2A) protocol v1.0 support — both directions.**
  Inbound: every tenant agent is reachable as an A2A endpoint via
  `POST /v1/a2a/agents/:id` (JSON-RPC `message/send`, `message/stream`,
  `tasks/get`, `tasks/cancel`); each agent's `AgentCard` is published at
  `GET /v1/a2a/agents/:id/agent-card.json`, with an unauth top-level
  discovery stub at `GET /.well-known/agent-card.json`. Outbound:
  arbiter agents can call remote A2A agents listed in
  `~/.arbiter/a2a_agents.json` via the new `/a2a list|card|call` slash
  command. The master orchestrator's preamble injects a
  `REMOTE A2A AGENTS` block alongside the local `AGENTS` roster so
  `index` can route to either side from the same routing decision.
  Tasks persist in a new `a2a_tasks` SQLite table; cancel hooks into
  the existing `InFlightRegistry` so `tasks/cancel` and
  `POST /v1/requests/:id/cancel` resolve through the same handle.
  Spec stance: v1.0 only; other versions rejected with
  `VersionNotSupportedError` (-32007). `tasks/resubscribe` and push
  notifications are deferred to v2; A2A `contextId` is currently not
  mapped to conversation rows so `/write --persist`, `/read`, and
  `/list` are unwired for A2A sessions (files still flow as
  `TaskArtifactUpdateEvent` frames). See
  [`docs/api/concepts/a2a.md`](docs/api/concepts/a2a.md) and
  [`docs/cli/a2a-agents.md`](docs/cli/a2a-agents.md).
- **`public_base_url` server option** for TLS-fronted deploys —
  determines the `url` field arbiter advertises in agent cards. Falls
  back to `http://<Host header>` when unset.
- **NEAR proximity in memory FTS queries**. For 2-to-6-token queries the
  FTS expression now emits a `NEAR("t1" "t2" ..., 8)` clause between
  the strict phrase and the OR fallback. Bridges the precision/recall
  gap — rows where tokens appear within 8 word positions of each other
  score above pure bag-of-words without a new index.
- **Score-aware (RRF) graduated merge**. `search_entries_graduated`
  now reciprocal-rank-fuses the conversation-scoped and tenant-wide
  passes (conv. weight 1.5, tenant weight 1.0, k=60) instead of
  appending-with-dedupe. A strong tenant-wide hit can outrank a weak
  conversation-local one, fixing the multi-session retrieval case
  where the answer lives in another conversation. Pool widened to
  `max(3·cap, 50)` so fusion has overlap.
- **Rerank prompt enrichment**. Each candidate row in the rerank
  prompt now carries `(type · YYYY-MM-DD · superseded YYYY-MM-DD)`
  metadata (when fields are set). The reranker can pick the
  most-recent non-superseded entry on temporal/knowledge-update
  questions, prefer a typed entry that matches the question's intent,
  and skip explicitly invalidated rows. Mirrors the same metadata
  surface in `fmt_entry_line` so agents see what the reranker sees.
- **Question-intent routing** on `GET /v1/memory/entries` and the
  agent-side `/mem search`. Heuristic regex classifier maps cue words
  to memory-entry type boosts via the existing 1.3× BM25 multiplier.
  Cue → type map: preference cues → `user`/`feedback`,
  reference cues → `reference`, learning cues → `learning`,
  temporal cues → `project`. Caller-supplied `type=` always wins.
  Disable via `?intent=off` (HTTP) or `memory.intent_routing=false`
  (agent). Zero LLM cost.
- **Query expansion via `?expand=<model>`** on the HTTP entries
  endpoint, opt-in per request. Server calls the model once to
  generate 2 paraphrases of the query, runs all 3 variants through
  the full FTS pipeline, and reciprocal-rank-fuses the rankings
  (original at 1.0, paraphrases at 0.7). No-embedding alternative to
  dense retrieval — closes the recall gap on paraphrased queries.
  Failures (advisor unavailable, unparseable response) are benign:
  search proceeds with the original query alone and surfaces a `note`
  in the new `expansion` response block.
- **Auto-tagging via `auto_tag=<model>`** in the `POST /v1/memory/entries`
  body. Advisor extracts 2-4 lowercase hyphenated topical tags from
  `title` + `content`; merged into caller-supplied tags. Tags carry
  an 8× BM25 weight, so this is the single cheapest way to lift
  retrieval signal on agent ingest paths that would otherwise leave
  tags empty. Surfaced in the response's `auto_tag` block.
- **Auto-supersession via `supersede=<model>`** in the entry-create
  body. After the new entry is persisted, the advisor inspects the
  top-5 same-type FTS hits on the new title for direct contradictions
  and stamps `valid_to=now()` on flagged ids. Conservative prompt
  ("when in doubt, prefer 'none'") to bias against false positives.
  Surfaces inspected `candidates` and `invalidated` ids in the
  response's `supersede` block.
- **`created_at` override** in `POST /v1/memory/entries` body. Accepts
  epoch seconds (preferred) or milliseconds (auto-detected). Sets
  `created_at`, `updated_at`, and `valid_from` to the override.
  Backfilling historical transcripts now produces entries with their
  real authored timestamps so temporal queries see them at the right
  point in time rather than ingest time.
- **Per-agent `MemoryConfig`** in agent JSON. New `memory` block on
  the constitution carries four toggles:
  - `intent_routing` (default `true`) — heuristic intent boost on `/mem search`
  - `search_expand` (default `false`) — query expansion on `/mem search`
  - `auto_tag` (default `false`) — advisor-extracted tags on `/mem add entry`
  - `auto_supersede` (default `false`) — advisor-driven invalidation on contradictory writes
  Agents whose `memory.search_expand` / `auto_tag` / `auto_supersede`
  is on get advisor-driven memory enrichment automatically on every
  `/mem` operation, transparent to the agent's own reasoning loop.
  All shipped agents (`backend`, `devops`, `frontend`, `marketer`,
  `planner`, `research`, `reviewer`, `social`, `writer`) now have
  `/mem` in capabilities; the four with advisors configured (backend,
  devops, frontend, research) opt in to all three advisor-driven
  toggles.
- **Date and supersession markers in `/mem entries` and `/mem entry`
  output**. `fmt_entry_line` now renders
  `- #42  [project] (YYYY-MM-DD · superseded YYYY-MM-DD)  Title …`
  when fields are set. Agents reasoning about recency or "what was
  true when" no longer need an extra `/mem entry` round trip.

### Changed
- `StructuredMemoryWriter` callback signature gained a fourth
  parameter `caller_id` (mirrors the `StructuredMemoryReader`
  signature). Consumers that don't need per-agent behavior can ignore
  the parameter; the HTTP server's writer closure now reads the
  caller's Constitution to decide whether `auto_tag` / `auto_supersede`
  fire on the write. Test stubs updated.
- `TenantStore::create_entry` gained an optional
  `created_at_override` parameter (defaulted to 0 = wall-clock). All
  existing callers continue to compile unchanged.

### Bench
- LongMemEval ingest now consumes per-session `haystack_dates` from
  the dataset and stamps each turn at `session_date + 60·turn_idx`
  via the new `created_at` override. Without dated entries, every
  ingested row shared the ingest-time timestamp and temporal-reasoning
  questions had no signal to ground against.
- `grade.py` `format_context` now annotates each retrieved entry with
  its session marker and authored date. `--top-k` default raised
  5 → 10 (R@10 ≈ 92% vs R@5 ≈ 90%). Per-entry truncation 800 → 2000
  chars (matches what the reranker fine-pass sees server-side).
  Generator system prompt softened — abstain on "clearly does not
  contain the answer" rather than "does not contain", with a positive
  nudge to answer when the memory supports an inference.
- `query.py` now plumbs `created_at`, `source`, `valid_from`,
  `valid_to`, and `conversation_id` through to `grade.py`. New
  `--expand-model` flag exercises the query expansion path.
- README headline command updated to recommend Sonnet generator,
  `--rerank-fine-model`, top-k=10, and (optionally) `--expand-model`.

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
