# Arbiter feature roadmap

Living document: where Arbiter stands today and a prioritized path toward a complete 1.0.

### Known sharp edges

- Circuit breaker thresholds hard-coded
- Sandbox: Docker-only; idle reaper documented but not implemented; no workspace-root env
- Hard kill can still lose an unfinished model stream; loops die on exit; queue depth is dropped
- A2A push notifications unsupported; event routing still experimental
- Documentation drift
- TUI `/exec` is host shell, not sandboxed by default

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
- [ ] **Tunable circuit breaker-** `ARBITER_CIRCUIT_*` env
- [ ] **Sandbox completion-** Idle reaper; exec-timeout kill inside container
- [ ] **TUI sandbox path-** Opt-in Docker for interactive `/exec`. default remains confirm-gated host with clearer danger UX
- [ ] **CORS allowlist env** `ARBITER_CORS_ORIGINS` as a documented alternative to the proxy
- [ ] **Event routing for API-created agents-** Complete buildout of currrent experimental implementation

**Acceptance criteria:** 
- [ ] `--api` is honest for unattended use: reconnect, retry, sandbox, and metrics behave as docs claim.

### Phase 3 — Project-aware agents
- [ ] **Workspace / repo map writ-** Cheap structural index (tree + symbols/outline) injected or fetchable; not a full LSP server in-process if avoidable
- [x] **`/diff` + apply workflow-** First-class apply/reject for ` ```diff ` proposals; keep rendering; staged apply **with** undo; interactive `/diff` review (`[a]`/`[r]`); create missing files on apply without a separate write confirm
- [ ] **Git status surface-** Git branch, dirty files, last agent touches surfaced in session sidebar
- [ ] **PR helper agent pattern-** Agent constitution + MCP Github
- [ ] **Project lessons boostrap-** On first open of a cwd, optional scan of lessons/memory seeds

**Acceptance criteria:** 
- [ ] Reviewer/backend starters can navigate a mid-size repo without rediscovering layout every turn; users can accept agent patches without raw `/write` fear.

### Phase 4 — Provider & tool ergonomics
- [ ] **Model catalog UX-** Richer `/model` / `GET /v1/models` with context limits used by compaction
- [ ] **MCP setup UX-** Improve `--setup-tools`; TUI browser for enable/disable servers; clearer tool error cards and statuses
- [ ] **A2A pushNotificationConfig-** Scheduling bus for agent notifications

**Acceptance criteria:** 
- [x] New user setup zero to first message < 2 minutes

### Phase 5 — Multi-agent mission control
- [ ] **Fleet dashboard pane-** Live tree of depth, agent, tools, tokens; click-to-focus/Ctrl-W bindings
- [ ] **Plan to execution observability-** Planner plans as first-class objects with progress against todos
- [ ] **Delegation policies-** Consitutions declare max depth, allowed callees, budget caps (tokens/$)
- [ ] **Workflow recipes-** Checked-in “crews” (JSON): ordered/parallel graphs of agents + shared todo board
- [ ] **Advisor policy packs-** Reusable gate profiles (strict / coding / research)

**Acceptance criteria:** 
- [ ] A user can watch and steer a 5-agent job.

### Phase 6 — 1.0
- [ ] **Security defaults pass-** Safer TUI exec prompts; clearer sandbox docs; threat model refresh in SECURITY.md
- [ ] **Sandbox + compaction test suites-** Automated coverage for the 1.0 risk surfaces
- [ ] **Stability freeze-** Constitution / SSE / writ schema versioning; compatibility promises
- [ ] **1.0 declaration-** Drop “experimental” once A–B exit criteria met and schemas versioned
