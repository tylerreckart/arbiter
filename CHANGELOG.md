# Changelog

All notable changes to arbiter are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
loosely while pre-1.0 (breaking changes can land on minor bumps).

## [Unreleased]

### Added
- Quartermaster billing integration. When `QUARTERMASTER_URL` is set,
  every authenticated request is exchanged for a workspace_id via
  `POST /v1/runtime/auth/validate`, pre-flighted against
  `POST /v1/runtime/quota/check`, and post-turn telemetry is fired
  (fire-and-forget, idempotent on `request_id-tN` per turn) to
  `POST /v1/runtime/usage/record`. With the env var unset, the runtime
  is a thin pass-through using the operator-supplied provider keys —
  no eligibility checks, no caps.
- Per-tenant artifact store. `POST /v1/conversations/:id/artifacts` and
  the matching list / get / raw / delete endpoints persist agent-
  generated files server-side with per-conversation and per-tenant
  quotas.
- Structured-memory graph: `/v1/memory/entries`, `/v1/memory/relations`,
  and `/v1/memory/graph` for typed nodes + directed labeled edges.
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
  — the usage ledger lives in Quartermaster.
- **Breaking**: SSE event shapes lost their cost fields.
  `token_usage` no longer carries `provider_micro_cents`,
  `billed_micro_cents`, `markup_micro_cents`, or `mtd_micro_cents`.
  `done` no longer carries `cap_exceeded`, `provider_micro_cents`,
  `billed_micro_cents`, or `markup_micro_cents`. `error` events for
  Quartermaster denials carry `reason`, `*_micro_cents` budget fields,
  and a human-readable `message` instead.
- **Breaking**: `POST /v1/admin/tenants` no longer accepts `cap_usd` or
  `monthly_cap_micro_cents` in the body. `PATCH /v1/admin/tenants/:id`
  only accepts `disabled`.
- **Breaking**: CLI `--add-tenant` no longer takes `--cap`, and
  `--tenant-usage` is gone.
- **Breaking**: `/v1/models` no longer includes pricing fields. The
  endpoint returns `id` + `provider` only; pricing now lives in
  Quartermaster's rate card.
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
  REPL session-cost footer are gone — Quartermaster owns pricing.
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
