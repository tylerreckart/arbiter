# Arbiter feature roadmap

Living document: where Arbiter stands today, how it compares to peer TUIs and
agent orchestration tools, and a prioritized path toward a complete 1.0.

### Known sharp edges

Already called out in CHANGELOG / docs / code comments:

- Context summarization / trim — ([`docs/tui/sessions.md`](docs/tui/sessions.md))
- Idempotency cache in-memory only
- Circuit breaker thresholds hard-coded
- Sandbox: Docker-only; idle reaper documented but not implemented; no workspace-root env
- Pane layout not persisted; hard kill loses unsaved turns; loops die on exit
- A2A push notifications unsupported; event routing still experimental
- Documentation drift
- TUI `/exec` is host shell, not sandboxed by default

---

## Roadmap

Phased by dependency and philosophy fit. Versions are directional, not
calendar commitments.

### Phase 1 — Make long sessions survivable (1.0 blockers)
- [ ] **Context compaction / summarization-** Threshold-triggered summarize of older turns; preserve recent window + pinned facts; optional advisor-assisted summarize; keep full history on disk for replay 
- [ ] **Conversation autosave-** Periodic + post-turn save so SIGKILL doesn’t lose work
- [ ] **In-flight turn recovery (TUI)-** Mirror durable request log pattern into local conversations, or at least don’t drop completed tool results on quit
- [ ] **Doc drift pass-** Align sessions/scheduler/memory docs with shipped TUI parity; fix sandbox env docs vs code

**Acceptance criteria:** 
- [ ] Multi-hour multi-pane sessions survive restart and provider context limits without manual `/reset`.

### Phase 2 — Production-grade local server
- [ ] **Durable idempotency-** Persist `(tenant, key) → request_id` acriss restarts
- [ ] **Tunable circuit breaker-** `ARBITER_CIRCUIT_*` env
- [ ] **Sandbox completion-** Idle reaper; exec-timeout kill inside container
- [ ] **TUI sandbox path-** Opt-in Docker for interactive `/exec`. default remains confirm-gated host with clearer danger UX
- [ ] **CORS allowlist env** `ARBITER_CORS_ORIGINS` as a documented alternative to the proxy
- [ ] **Event routing for API-created agents-** Complete buildout of currrent experimental implementation

**Acceptance criteria:** 
- [ ] `--api` is honest for unattended use: reconnect, retry, sandbox, and metrics behave as docs claim.

### Phase 3 — Project-aware agents
- [ ] **Workspace / repo map writ-** Cheap structural index (tree + symbols/outline) injected or fetchable; not a full LSP server in-process if avoidable
- [ ] **`/diff` + apply workflow-** First-class apply/reject for ` ```diff ` proposals; keep rendering; add staged apply **with** undo
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
- [ ] New user setup zero to first message < 2 minutes

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

---

## Deliberate non-goals

Keep these out unless philosophy changes — they dilute the product:

| Non-goal | Why |
|----------|-----|
| Becoming a Python/TS agent **library** | Arbiter is a process you run |
| Built-in TLS / WAF / global rate limit | Reverse proxy owns ops ([philosophy §6](docs/philosophy.md)) |
| Hosted multi-tenant SaaS as the default product | Local-first; `--api` is self-hosted |
| JSON tool-calling as the primary agent interface | Writ DSL is the point |
| Full IDE / Electron desktop (Goose-style) as core | TUI + HTTP clients (e.g. Newton) cover surfaces |
| Replacing git with a custom VCS UI | Integrate; don’t reimplement |
| Plugin/WASM marketplace before 1.0 | Extension via MCP/A2A/constitutions is enough until schemas stabilize |
| Real-time multi-user CRDT TUI | Tenants + A2A are the collab path |

---

## Success metrics

Use these to decide whether a phase actually “completed” Arbiter:

1. **Survive the afternoon** — 4+ hour TUI session with parallel panes, no forced `/reset`, restart restores layout + history.
2. **Unattended hour** — `--api` job with sandbox + schedule + reconnect after process restart; idempotent client retry safe.
3. **Repo PR loop** — reviewer+backend crew proposes diff → user applies → tests via `/exec` → commit via git/MCP without leaving Arbiter.
4. **Cold start** — new machine, one key, `--init`, first specialist reply under five minutes.
5. **Fleet glance** — user can answer “which agent is blocked on which tool?” in one screen.

