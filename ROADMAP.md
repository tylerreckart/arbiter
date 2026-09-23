# Arbiter feature roadmap

Living document: where Arbiter stands today and a prioritized path toward a complete 1.0.

### Known sharp edges

- Sandbox: Docker-only (`ARBITER_SANDBOX_RUNTIME` accepts other values but only docker is supported)
- Hard kill can still lose an unfinished model stream; loops die on exit; queue depth is dropped
- A2A push notifications unsupported
- Documentation drift (narrower than before Phase 2; keep watching ops/sandbox docs)

---

## Roadmap

Phased by dependency and philosophy fit. Versions are directional, not
calendar commitments.

### Phase 1 — Make long sessions survivable (1.0 blockers)
- [x] **Context compaction / summarization-** Threshold-triggered summarize of older turns; preserve recent window + pinned facts; optional advisor-assisted summarize; keep full history on disk for replay 
- [x] **Conversation autosave-** Periodic + post-turn save so SIGKILL doesn’t lose work
- [x] **In-flight turn recovery (TUI)-** Mirror durable request log pattern into local conversations, or at least don’t drop completed tool results on quit
- [x] **Doc drift pass-** Align sessions/scheduler/memory docs with shipped TUI parity; fix sandbox env docs vs code

**Acceptance criteria:** 
- [x] Multi-hour multi-pane sessions survive restart and provider context limits without manual `/reset`.
- [x] Restart restores the prior split layout and per-pane conversation bindings (`layout.json`).

### Phase 2 — Production-grade local server
- [x] **Durable idempotency-** Persist `(tenant, key) → request_id` across restarts
- [x] **Tunable circuit breaker-** `ARBITER_CIRCUIT_FAILURE_THRESHOLD` / `ARBITER_CIRCUIT_COOLDOWN_SECONDS`
- [x] **Sandbox completion-** Idle reaper (structured log); in-container `timeout` + survivor kill; `ARBITER_SANDBOX_WORKSPACES_ROOT`
- [x] **TUI sandbox path-** Opt-in Docker for interactive `/exec` via `ARBITER_SANDBOX_IMAGE`; default remains confirm-gated host with `HOST SHELL (unsandboxed)` danger UX
- [x] **CORS allowlist env** `ARBITER_CORS_ORIGINS` as a documented alternative to the proxy
- [x] **Event routing for API-created agents-** File-backed then tenant-stored agents (`event_types`); explicit `agent` override unchanged

**Acceptance criteria:** 
- [x] `--api` is honest for unattended use: reconnect, retry, sandbox, and metrics behave as docs claim.

### Phase 3 — Project-aware agents
- [x] **Workspace / repo map writ-** Cheap structural index (`/map` tree; outline/inject follow-up); not a full LSP server in-process if avoidable
- [x] **`/diff` + apply workflow-** First-class apply/reject for ` ```diff ` proposals; keep rendering; staged apply **with** undo; interactive `/diff` review (`[a]`/`[r]`); create missing files on apply without a separate write confirm
- [ ] **Git status surface-** Git branch, dirty files, last agent touches surfaced in session sidebar
- [ ] ~~*PR helper agent pattern-** Agent constitution + MCP Github~~
- [ ] **Project lessons boostrap-** On first open of a cwd, optional scan of lessons/memory seeds

**Acceptance criteria:** 
- [ ] Reviewer/backend starters can navigate a mid-size repo without rediscovering layout every turn; users can accept agent patches without raw `/write` fear.

### Phase 4 — Provider & tool ergonomics
- [x] **Model catalog UX-** Richer `/model` / `GET /v1/models` with context limits used by compaction
- [ ] **MCP setup UX-** Improve `--setup-tools`; TUI browser for enable/disable servers; clearer tool error cards and statuses
- [ ] **A2A pushNotificationConfig-** Scheduling bus for agent notifications
- [x] **Spoken voice channel-** `mode: "spoken"` + request `channel: "voice"` so Intercom-style bridges get conversational TTS prose without stuffing reminders into user turns ([docs](docs/concepts/voice.md)). In-process STT/TTS remains [#207](https://github.com/tylerreckart/arbiter/issues/207)

**Acceptance criteria:** 
- [x] New user setup zero to first message < 2 minutes

### Phase 5 — Multi-agent mission control
- [x] **Intent engine foundations-** Hybrid classify/route before dispatch (heuristic, then advisor-model LLM); seed slots for plans/todos; fail-open into index; `POST /v1/intent` + SSE `intent` event
- [x] **Intent reconcile (Phase A)-** `POST /v1/reconcile`: compile `target_state` + invariants into a state contract, observe a bound workspace, require verification, optional snapshot rollback; `@arbiter/sdk` `IntentClient`; durable `reconcile_runs` + SSE
- [x] **JIT ΔS waves (Phase B / #208)-** `mode=ensure` wave loop: resolve `agent_map` / clause.agent / capability fallback, spawn ephemeral clones for active ΔS only, re-observe as success proof, teardown before the next wave; budgets `max_waves` / `max_wall_ms` / `max_agents_per_wave` / `max_retries_per_clause`; SSE `agent.spawned` / `agent.teardown`. Observe mode and `/v1/orchestrate` unchanged
- [x] **Always-on presence-** Constitution `presence.mode: always_on`; a pair colleague looks over a peer's shoulder after each tool batch and may inject `[PRESENCE: …]` context. Fail-open; cannot halt. SSE `presence` + TUI `◎ presence`. Opposite lifetime of JIT (#208).
- [x] **Fleet dashboard pane-** Live tree of depth, agent, tools, tokens; click-to-focus / `Ctrl-w f` ([docs](docs/tui/fleet.md))
- [ ] **Plan to execution observability-** Planner plans as first-class objects with progress against todos
- [ ] **Delegation policies-** Consitutions declare max depth, allowed callees, budget caps (tokens/$)
- [ ] ~~**Workflow recipes-** Checked-in “crews” (JSON): ordered/parallel graphs of agents + shared todo board~~
- [ ] **Advisor policy packs-** Reusable gate profiles (strict / coding / research)
- [ ] **Decision-filter measurement-** Refit `ARBITER_PRESENCE_SILENCE_MARGIN` and `ARBITER_INTENT_ROUTE_MARGIN` from `filter_*` / `decision_*` traces. The advisor gate stays off this path until those floors exist.

**Acceptance criteria:** 
- [x] A user can watch and steer a 5-agent job.

### Phase 6 — 1.0
- [ ] **Security defaults pass-** Safer TUI exec prompts; clearer sandbox docs; threat model refresh in SECURITY.md
- [ ] **Sandbox + compaction test suites-** Automated coverage for the 1.0 risk surfaces
- [ ] **Stability freeze-** Constitution / SSE / writ schema versioning; compatibility promises
- [ ] **1.0 declaration-** Drop “experimental” once A–B exit criteria met and schemas versioned
