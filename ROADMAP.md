# Arbiter feature roadmap

Living document: where Arbiter stands today, how it compares to peer TUIs and
agent orchestration tools, and a prioritized path toward a complete 1.0.

Aligned with [`docs/philosophy.md`](docs/philosophy.md): thin runtime, writ
DSL, one binary / three faces, hard capability gates, local-first. Items that
would break those constraints are listed under **Deliberate non-goals**.

Status snapshot: **v0.8.x, experimental / pre-1.0**.

---

## 1. What Arbiter is today

A **local-first multi-agent runtime** shipped as a single native C++ binary.
Agents are JSON constitutions (model, role, rules, hard tool allowlists). They
act by emitting a prose-embedded slash-command DSL (“writs”), not JSON
tool-calling. Three faces share one event model and `~/.arbiter/` storage:

| Face | Entry | Strength |
|------|-------|----------|
| TUI | `arbiter` | Multi-pane OpenTUI, themes, activity timeline, sidebars |
| One-shot | `arbiter --send` | Scripts, cron, CI |
| Server | `arbiter --api` | HTTP+SSE, tenants, MCP, A2A v1.0 subset |

### Current feature inventory

| Area | Shipped |
|------|---------|
| **Agents** | Master `index` + file-backed specialists; starters (backend, devops, frontend, marketer, planner, research, reviewer, social, writer); `/agents`, `/use`, `/create`, `/remove`, `/model` |
| **Orchestration** | Sync `/agent`, `/parallel`…`/endparallel`, detached `/pane`, depth-capped fleet streaming, background `/loop` lifecycle, advisor consult + structural gate (`CONTINUE` / `REDIRECT` / `HALT`), experimental `POST /v1/events` routing |
| **TUI** | Split/zoom/cycle panes, history + session sidebars, markdown + inline diffs, 38 embedded themes, mouse + Kitty keyboard, tab-complete, permission cards, activity timeline |
| **Providers** | OpenRouter (hosted), Ollama (`ollama/…`); Anthropic / OpenAI-compat / Gemini wire formats; vision via image parts; token/cost tracking; circuit breaker |
| **Tools** | Writs: fetch, search, browse, read, list, exec, write, mem, mcp, a2a, advise, todo, schedule, lesson, agent, parallel, pane; hard allowlists; confirm gates; SSRF guards |
| **Protocols** | MCP client (stdio + mcp-remote); A2A v1.0 inbound/outbound (pushNotificationConfig unsupported) |
| **Persistence** | Global conversations, soft-delete/purge, structured memory graph (FTS5/BM25, relations, decay), scratchpads, lessons, todos, scheduler + notifications, durable request event log + reconnect |
| **API ops** | Multi-tenant bearer auth, admin + audit log, Prometheus metrics, health, graceful drain, rate/concurrency limits, optional Docker sandbox for API `/exec` |
| **Extensibility** | Agent JSON, MCP servers, A2A remotes, themes, constitutions — **no** plugin/WASM/Lua runtime |

### Known sharp edges (in-repo)

Already called out in CHANGELOG / docs / code comments:

- Context summarization / trim — **explicitly not done** ([`docs/tui/sessions.md`](docs/tui/sessions.md))
- Idempotency cache in-memory only (Phase 3 follow-up)
- Circuit breaker thresholds hard-coded (Phase 5 follow-up)
- Sandbox: Docker-only; idle reaper documented but not implemented; no workspace-root env
- Pane layout not persisted; hard kill loses unsaved turns; loops die on exit
- A2A push notifications unsupported; event routing still experimental
- Doc drift (scheduler / memory graph “API-only” notes vs TUI parity in later releases)
- TUI `/exec` is host shell (confirm-gated), not sandboxed by default

---

## 2. Competitive landscape

Arbiter sits in an unusual intersection: **TUI coding-agent UX** × **multi-agent runtime** × **local HTTP server**. Most peers optimize one corner.

### Agent TUIs / CLIs (coding-first)

| Tool | Positioning | Arbiter relative |
|------|-------------|------------------|
| **Claude Code** | First-party Anthropic agent; strongest autonomous coding loop | Behind on single-agent coding polish, permissions UX, subagent maturity for *code*; ahead on multi-agent constitutions, memory graph, tenants, A2A, local server |
| **OpenCode** | Open multi-provider TUI; Mission Control / session sync | Behind on provider breadth & coding-agent ergonomics; ahead on hard allowlists, advisor gate, structured memory, HTTP+SSE runtime |
| **Crush** | Charm-polished TUI; LSP-aware context | Behind on LSP/repo intelligence & TUI craft reputation; ahead on orchestration depth and server face |
| **Aider** | Git-native pair programmer; repo map; auto-commits | Behind on git workflow & repo mapping; different product (Arbiter is a runtime, not a git porcelain) |
| **Goose** | Desktop + CLI + API; deep MCP ecosystem; ACP | Behind on extension marketplace & desktop surface; closer philosophically (local agent + MCP + API); Arbiter deeper on multi-agent writs / advisor / tenants |
| **Gemini CLI / Codex CLI** | Vendor CLIs | Same pattern: coding agent, not orchestration runtime |

### Orchestration frameworks (library-first)

| Tool | Positioning | Arbiter relative |
|------|-------------|------------------|
| **CrewAI** | Role-based crews + flows (Python) | Similar mental model (specialists + tasks); Arbiter is a **process**, not a library — ships TUI/API instead of embedding |
| **LangGraph** | Typed state graphs, checkpointing, HITL | Stronger durable workflow graphs & observability; Arbiter stronger as runnable product with panes/SSE/tenants |
| **Microsoft Agent Framework / AutoGen** | Enterprise orchestration, Magentic-One patterns | Stronger enterprise SDK story; Arbiter lighter, local-first, constitution+writ shaped |
| **OpenHands** | Sandboxed coding agent + browser/VS Code in container | Stronger sandbox/browser/IDE agent-computer interface; Arbiter stronger multi-agent federation & TUI |

### Positioning map

```text
                    coding-agent polish
                            ▲
              Claude Code   │   OpenCode / Crush
              OpenHands     │   Goose
                            │
   library  ◄───────────────┼───────────────►  product binary
   LangGraph CrewAI MAF     │              Arbiter
                            │
                            ▼
                    multi-agent runtime / federation
```

**Arbiter’s moat today:** one binary that is simultaneously a serious multi-pane
TUI *and* a tenant-isolated agent runtime with hard allowlists, advisor gates,
structured memory/lessons/todos/schedules, MCP, and A2A — without becoming a
Python framework.

**Arbiter’s gaps vs category leaders:** long-context survival, git/repo
intelligence, provider ergonomics, sandbox completeness, and “mission control”
visibility for multi-agent work.

---

## 3. Gap analysis (what “complete” means)

Completeness for Arbiter ≠ matching Claude Code feature-for-feature. It means
closing gaps that block daily use of the **runtime thesis**, while doubling
down on what peers lack.

| Gap | Why it matters | Peer pressure |
|-----|----------------|---------------|
| **G1. Context compaction** | Long multi-agent threads die against provider limits; `/reset` is blunt | OpenHands, Claude Code, Goose all compress/summarize |
| **G2. Session durability** | Pane layout, in-flight turns, autosave, loop survival | OpenCode session sync; any serious TUI expects layout restore |
| **G3. Sandbox + host safety** | TUI `/exec` on host; sandbox gaps vs docs | OpenHands Docker/VNC; every production agent story |
| **G4. Repo / project intelligence** | Agents re-discover structure via `/list`/`/read` every time | Aider repo map; Crush LSP; Claude Code project memory |
| **G5. Provider surface** | OpenRouter+Ollama works, but direct keys / subscription auth expected | OpenCode 75+ providers; Goose ACP subscriptions |
| **G6. Git-native change workflow** | Diffs render well; commit/PR loop is shell+MCP improvisation | Aider auto-commit; Claude Code PR flows |
| **G7. Multi-agent observability** | Fleet streaming exists; operator UX for “what is the crew doing?” is thin | OpenCode Mission Control; LangGraph/LangSmith |
| **G8. Durable API correctness** | Idempotency + crash story incomplete for serious `--api` users | LangGraph checkpointing; any SaaS agent API |
| **G9. Protocol completeness** | A2A push, event routing maturity, MCP UX | Goose MCP depth; A2A spreading across clouds |
| **G10. Docs / 1.0 trust** | Experimental label, doc drift, missing tests for sandbox | Trust tax vs Goose/OpenCode polish |

---

## 4. Roadmap

Phased by dependency and philosophy fit. Versions are directional, not
calendar commitments.

### Phase A — Make long sessions survivable (1.0 blockers)

*Closes G1, G2, parts of G10.*

| ID | Item | Notes |
|----|------|-------|
| A1 | **Context compaction / summarization** | Threshold-triggered summarize of older turns; preserve recent window + pinned facts; optional advisor-assisted summarize; keep full history on disk for replay |
| A2 | **Conversation autosave** | Periodic + post-turn save so SIGKILL doesn’t lose work |
| A3 | **Persist pane layout** | Restore splits / agent bindings / zoom on relaunch |
| A4 | **In-flight turn recovery (TUI)** | Mirror durable request log pattern into local conversations, or at least don’t drop completed tool results on quit |
| A5 | **Doc truth pass** | Align sessions/scheduler/memory docs with shipped TUI parity; fix sandbox env docs vs code |

**Exit criteria:** Multi-hour multi-pane sessions survive restart and provider context limits without manual `/reset`.

### Phase B — Production-grade local server

*Closes G3, G8, remaining CHANGELOG “Phase 3/5” items.*

| ID | Item | Notes |
|----|------|-------|
| B1 | **Durable idempotency** | Persist `(tenant, key) → request_id` across restarts (CHANGELOG Phase 3) |
| B2 | **Tunable circuit breaker** | `ARBITER_CIRCUIT_*` env (Phase 5) |
| B3 | **Sandbox completion** | Idle reaper; exec-timeout kill inside container; `ARBITER_SANDBOX_WORKSPACE` (or equiv); tests for lifecycle |
| B4 | **Optional TUI sandbox path** | Opt-in Docker (or landlock/bubblewrap later) for interactive `/exec`, default remains confirm-gated host with clearer danger UX |
| B5 | **CORS allowlist env** | `ARBITER_CORS_ORIGINS` as documented alternative to “edit the proxy” |
| B6 | **Event routing for API-created agents** | Lift experimental limitation (file-backed only) |

**Exit criteria:** `--api` is honest for unattended use: reconnect, retry, sandbox, and metrics behave as docs claim.

### Phase C — Project-aware agents (without becoming an IDE)

*Closes G4, G6 — stay writ-shaped.*

| ID | Item | Notes |
|----|------|-------|
| C1 | **Workspace / repo map writ** | Cheap structural index (tree + symbols/outline) injected or fetchable; not a full LSP server in-process if avoidable — MCP LSP bridge is fine |
| C2 | **`/diff` + apply workflow** | First-class apply/reject for ` ```diff ` proposals; keep rendering; add staged apply with undo |
| C3 | **Git status surface** | Sidebar or `/status` section: branch, dirty files, last agent touches — still shell underneath |
| C4 | **PR helper agent pattern** | Starter constitution + MCP GitHub, not a built-in GitHub App |
| C5 | **Project lessons bootstrap** | On first open of a cwd, optional scan → lessons/memory seeds (stack, test cmd, lint) |

**Exit criteria:** Reviewer/backend starters can navigate a mid-size repo without rediscovering layout every turn; users can accept agent patches without raw `/write` fear.

### Phase D — Provider & tool ergonomics

*Closes G5, parts of G9.*

| ID | Item | Notes |
|----|------|-------|
| D1 | **Direct Anthropic / OpenAI / Gemini keys** | Keep OpenRouter; add first-class env paths so “works with my existing key” is one export |
| D2 | **Model catalog UX** | Richer `/model` / `GET /v1/models` with context limits used by compaction (A1) |
| D3 | **MCP setup UX** | Improve `--setup-tools`; TUI browser for enable/disable servers; clearer tool error cards |
| D4 | **A2A pushNotificationConfig** | Or document permanent non-support and offer webhook notifications via existing schedule bus |
| D5 | **Hosted MCP / OAuth flows** | Only where they stay optional and local-first |

**Exit criteria:** New user with only an Anthropic or OpenAI key reaches first successful multi-agent turn in minutes.

### Phase E — Multi-agent mission control

*Closes G7; amplifies Arbiter’s differentiator.*

| ID | Item | Notes |
|----|------|-------|
| E1 | **Fleet dashboard pane** | Live tree of depth, agent, tool, tokens; click-to-focus child pane |
| E2 | **Plan → execute observability** | Planner plans as first-class objects with progress against todos |
| E3 | **Delegation policies** | Constitutions declare max depth, allowed callees, budget caps (tokens/$) beyond today’s allowlists |
| E4 | **Workflow recipes** | Checked-in “crews” (JSON): ordered/parallel graphs of agents + shared todo board — still not a Python DSL |
| E5 | **Advisor policy packs** | Reusable gate profiles (strict / coding / research) |

**Exit criteria:** A user can watch and steer a 5-agent job the way OpenCode Mission Control markets — but with Arbiter’s allowlists and advisor semantics.

### Phase F — Trust, packaging, 1.0

*Closes G10.*

| ID | Item | Notes |
|----|------|-------|
| F1 | **Security defaults pass** | Safer TUI exec prompts; clearer sandbox docs; threat model refresh in SECURITY.md |
| F2 | **Sandbox + compaction test suites** | Automated coverage for the 1.0 risk surfaces |
| F3 | **Stability freeze** | Constitution / SSE / writ schema versioning; compatibility promises |
| F4 | **1.0 declaration** | Drop “experimental” once A–B exit criteria met and schemas versioned |

---

## 5. Priority stack (recommended sequence)

```text
Now (pre-1.0 spine)
  A1 Context compaction
  A2 Autosave
  B1 Durable idempotency
  B3 Sandbox completion
  A5 Doc truth pass

Next (daily-driver quality)
  A3 Pane layout persistence
  C2 Diff apply/reject
  D1 Direct provider keys
  D3 MCP setup UX
  E1 Fleet dashboard

Then (moat expansion)
  C1 Repo map
  E3–E4 Delegation policies + workflow recipes
  B4 Optional TUI sandbox
  E2 Plan objects

Later / demand-gated
  D4 A2A push (or webhook substitute)
  B5 CORS env
  Metrics aggregate-by-tenant
  Non-Docker sandbox runtimes
```

---

## 6. Deliberate non-goals

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

## 7. Success metrics (product, not vanity)

Use these to decide whether a phase actually “completed” Arbiter:

1. **Survive the afternoon** — 4+ hour TUI session with parallel panes, no forced `/reset`, restart restores layout + history.
2. **Unattended hour** — `--api` job with sandbox + schedule + reconnect after process restart; idempotent client retry safe.
3. **Repo PR loop** — reviewer+backend crew proposes diff → user applies → tests via `/exec` → commit via git/MCP without leaving Arbiter.
4. **Cold start** — new machine, one key, `--init`, first specialist reply under five minutes.
5. **Fleet glance** — user can answer “which agent is blocked on which tool?” in one screen.

---

## 8. How to use this document

- Treat Phase **A/B** as the 1.0 contract; C–E as differentiation; F as graduation.
- When proposing features, cite a gap ID (`G1`…) or roadmap ID (`A1`…).
- If a proposal conflicts with §6, it needs an explicit philosophy amendment first.
- Update this file when a phase’s exit criteria land (move rows to a short “Done” section rather than deleting history).

### Suggested near-term implementation slices

Smallest valuable increments that unlock later work:

1. **Autosave + compaction design doc** (A2 then A1) — compaction needs durable history to be useful.
2. **Sandbox doc/code convergence + idle reaper** (B3) — restores operator trust.
3. **Diff apply writ** (C2) — high user-visible payoff, localized change.
4. **Fleet sidebar v1** (E1) — surfaces orchestration moat in the TUI.

---

*Comparisons reflect publicly documented capabilities of peer tools as of mid-2026 and Arbiter’s in-repo surface at the time of writing. Peers move quickly; re-validate before large bets.*
