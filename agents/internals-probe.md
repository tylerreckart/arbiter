# Arbiter Internals Probe

Use this prompt with a coding agent (Cursor cloud agent, local agent, or
Arbiter `auditor` constitution) to systematically stress Arbiter's internals
and emit **GitHub-ready issues**: required fixes and high-value enhancements.

Copy everything under **Prompt** into the agent. Keep this file as the
canonical version.

---

## Prompt

You are probing **Arbiter** (`tylerreckart/arbiter`): a native C++20 agent
runtime with a TUI, one-shot CLI, and multi-tenant HTTP+SSE API. One binary;
shared storage under `~/.arbiter/`; agents act through a writ DSL (`/fetch`,
`/exec`, `/mem`, …) gated by per-agent capability allowlists.

### Mission

Produce a ranked backlog of **concrete GitHub issues** that a maintainer can
open or file privately. Prefer findings that are:

1. **Correctness / crash / memory-safety** bugs with a plausible trigger
2. **Security** bugs in-scope per `SECURITY.md` (authz bypass, tenant leak,
   SSRF, injection, credential exposure, sandbox boundary holes)
3. **High-value reliability or operability** gaps that burn operators or
   cause silent data loss
4. **Test / observability** holes that leave the above classes unguarded

Do **not** pad the backlog with style nits, rename bikesheds, or "add more
comments" unless a missing comment hides a real invariant.

### Non-goals and disclosure rules

Read and obey `SECURITY.md` and `CONTRIBUTING.md` before filing anything.

**Never open a public GitHub issue for a security finding.** For in-scope
security issues, write the issue body as if for private vulnerability
reporting, mark it `disclosure: private`, and stop — do not push a public
issue, PR description, or commit that includes exploit detail.

Out of scope as *vulnerabilities* (do not file as security; may note as
docs/ops enhancements if the docs are wrong):

- Demonstrating that unsandboxed `/exec` in the TUI runs with user privileges
- Missing TLS / rate limits / DDoS on the HTTP server (proxy expected)
- Provider cost spikes
- Upstream Docker/kernel breakouts (unless Arbiter's *own* sandbox boundary
  is wrong: workspace path traversal, cross-tenant mount, sandbox-disable
  bypass)

Philosophy constraints (`docs/philosophy.md`): do not propose major
architecture rewrites, in-runtime billing, or new unsandboxed tool surfaces
without framing them as discussion issues first.

### How to work

1. **Orient from docs, then verify in code.** Start with
   `SECURITY.md`, `docs/philosophy.md`, `docs/concepts/{writ,sandbox,authentication,tenants,operations,mcp,a2a,sse-events,durable-execution,artifacts,structured-memory}.md`,
   then the matching `include/` + `src/` implementation. Treat docs as
   claims to falsify.
2. **Follow trust boundaries**, not file size. Trace one request / writ /
   event from ingress to persistence to egress.
3. **Prefer evidence over vibes.** For each finding: cite file + function
   (and line range when stable), name the failing invariant, describe a
   minimal trigger, and state expected vs actual behavior.
4. **Check existing tests.** Look under `tests/` and `CHANGELOG.md` for
   prior fixes (UAF, TUI segfaults, parallel invokers). Prefer issues that
   add a missing regression test over "please be careful" advice.
5. **Build and run when the environment allows.**
   `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && ctest --test-dir build --output-on-failure`
   Prefer ASan/UBSan or Debug builds for crash hunting when available.
   Security-sensitive surfaces: `src/commands.cpp`, `src/tenant_store.cpp`,
   `src/sandbox.cpp`, `src/api_server.cpp`, `src/mcp/*`, `src/a2a/*`.
6. **Depth over breadth.** Finish at least one full pass on the Priority
   surfaces below before grazing low-risk UX polish. Cap total public
   issues at ~12–20 high-signal items unless the run is explicitly a
   full audit; merge related symptoms into one issue when they share a root
   cause.
7. **Do not implement fixes in this pass** unless asked. The deliverable is
   issues, not a drive-by refactor.

### Priority surfaces (probe in this order)

#### P0 — Security & isolation

| Surface | Primary code | What to falsify |
|---|---|---|
| Tenant authn/authz | `src/api_server.cpp`, `src/tenant_store.cpp`, `docs/concepts/authentication.md` | Bearer confusion; admin token accepted on non-admin routes; disabled tenant still serves; IDOR where one tenant's conversation/artifact/memory/agent/todo/schedule/lesson/request id returns another's data (must be `404`, never leak); timing leaks on token compare |
| Token & key hygiene | `tenant_store`, logger, SSE emitters, admin audit | Plaintext tenant/admin/provider keys in logs, errors, SSE, artifacts, `admin_audit.after`, crash dumps |
| SSRF / outbound fetch | `src/commands.cpp` (`is_blocked_address`, `/fetch`, `/search`, redirects) | Bypass via DNS rebinding, redirect-to-internal, IPv6/mapped forms, URL parsing quirks, MCP HTTP transport, A2A client |
| Path / SQL injection | `commands`, `tenant_store`, artifacts, sandbox workspace | User fields reaching SQL unsafely; `..` / absolute paths escaping artifact or workspace roots; symlink tricks on workspace |
| Sandbox boundary | `src/sandbox.cpp`, `include/sandbox.h`, `docs/concepts/sandbox.md` | Workspace canonicalization escape; cross-tenant `t<tid>` access; `/exec` reaching host when sandbox "enabled"; caps/`network=none` silently dropped; race on container ensure/stop |
| Capability gating | `constitution`, `orchestrator`, `commands` | Writ dispatched despite missing capability; API vs TUI policy mismatch (`exec_disabled`); advisor gate bypass on consequential turns |
| MCP / A2A federation | `src/mcp/*`, `src/a2a/*` | Subprocess env leakage; argument injection; SSRF via remote cards; task id cross-tenant; hostile SSE/event shapes crashing the translator |

#### P1 — Crashes, memory safety, concurrency

| Surface | Primary code | Failure modes |
|---|---|---|
| Orchestrator lifecycle | `src/orchestrator.cpp`, `include/orchestrator.h` | Cancel vs in-flight tool; parallel `/agent` history races; `agents_mutex_` / `parallel_clients_mu_` coverage holes; use-after-free of callbacks or `ApiClient`; throw crossing thread boundaries |
| API connection model | `api_server.cpp`, `InFlightRegistry` | Dangling `Orchestrator*` after handler exit; drain/shutdown vs active SSE; double-complete; unbounded buffering → OOM despite `file_max_bytes` |
| TUI / OpenTUI | `src/tui/**`, `src/main.cpp`, `src/repl/**` | Degenerate layout (0×0, negative origins — known `bufferDrawTextBufferView` class); resize mid-stream; theme swap races (`theme.cpp` notes string data races); Kitty/mouse decode overflows |
| JSON / parsers | `src/json.cpp`, `schedule_parser`, writ block parsers, constitution load | Deep nesting, giant strings, invalid UTF-8, truncated `/write` without `/endwrite`, hostile agent JSON |
| SQLite store | `tenant_store.cpp` | Busy/locked handling; partial migrations; cascade-delete integrity; concurrent writers from scheduler tick + request threads |
| Subprocesses | `sandbox.cpp`, `mcp/subprocess.cpp`, `cmd_exec` | Zombies; kill races; output cap bypass; signal mishandling |

Probe with adversarial inputs: empty ids, huge payloads, concurrent identical idempotency keys, cancel-during-tool, rapid pane open/close, malformed SSE from upstream, agent constitutions with weird capabilities/event_types globs.

#### P2 — Reliability & durable execution

- Scheduler tick thread vs request threads (`scheduler.cpp`, schedules API)
- Idempotency cache correctness under retry (`idempotency_cache.*`)
- Circuit breaker / provider failure degradation (`circuit_breaker.*`, `api_client.cpp`)
- Request event bus / notification bus ordering vs docs (`sse-events`, `fleet-streaming`, `durable-execution`)
- Conversation titling workers and other detached threads (CHANGELOG has prior UAF — hunt siblings)
- Artifact quota math on replace; memory soft-invalidate vs hard-delete semantics

#### P3 — High-value enhancements (only if clearly justified)

File as `enhancement`, not `bug`, and require a sharp user/operator payoff:

- Missing regression tests for a P0/P1 hazard you could only reason about
- Operability: clearer sandbox failure logs, metrics gaps for tenant isolation denials, kill-switch visibility
- Hardening beyond current guarantees (e.g. stricter redirect policy) — separate from claiming a vuln exists
- Doc/implementation drift where operators would misconfigure a security boundary

Skip cosmetic TUI chrome, welcome card polish, and theme preset churn
(`CONTRIBUTING.md` calls these out of scope).

### Investigation checklist (run explicitly)

For each P0/P1 surface you touch, answer in your working notes:

- [ ] What is the trust boundary and who is the attacker (tenant, agent model, MCP server, A2A peer, local TUI user)?
- [ ] What invariant does the code/docs claim?
- [ ] Is the invariant enforced in *every* path (TUI, `--send`, `--api`, scheduler-fired runs, A2A dispatch)?
- [ ] What happens on cancel, timeout, exception, and process crash mid-operation?
- [ ] Is there a test that would fail if the bug existed? If not, specify one.
- [ ] Is this already fixed on `main` / mentioned in `CHANGELOG.md`?

### Issue quality bar

Each candidate issue must include:

| Field | Requirement |
|---|---|
| Title | Imperative, specific, ≤72 chars (`Fix …`, `Harden …`, `Reject …`, `Add test for …`) |
| Type | `bug` · `crash` · `security` · `enhancement` · `test` · `docs` |
| Severity | `critical` (exploit/crash/data leak) · `high` · `medium` · `low` |
| Disclosure | `public` or `private` |
| Component | one of: `api-server`, `orchestrator`, `commands`, `tenant-store`, `sandbox`, `tui`, `mcp`, `a2a`, `scheduler`, `api-client`, `constitution`, `docs`, `tests` |
| Affected modes | subset of `tui`, `cli-send`, `api`, `scheduler` |
| Evidence | file paths + symbols; brief code citation |
| Reproduction | minimal steps or hypothesis with trigger conditions if not yet runtime-confirmed |
| Expected / actual | invariant vs observed/reasoned behavior |
| Impact | who gets hurt and how |
| Suggested fix direction | concrete, not "improve validation" |
| Test plan | new/extended `ctest` name or scenario |
| Out-of-scope note | if a reader might confuse it with documented unsandboxed behavior |

**Reject** candidates that are pure speculation with no code anchor, duplicates of CHANGELOG-fixed items, or security reports that only restate documented `/exec` power.

### Output format

Return two artifacts in your final response:

#### 1. Executive summary (≤15 lines)

- Scope covered / not covered
- Counts by severity and disclosure
- Top 3 issues to act on first

#### 2. Issue dossier

For each finding, emit a fenced block ready to paste into GitHub (or private advisory). Use exactly this template:

```markdown
### <title>

- **Type:** bug | crash | security | enhancement | test | docs
- **Severity:** critical | high | medium | low
- **Disclosure:** public | private
- **Component:** <component>
- **Modes:** <modes>
- **Status:** confirmed | likely | needs-repro

#### Summary
<2–4 sentences>

#### Evidence
- `<path>:<symbol or lines>` — <what it shows>

#### Reproduction
1. …
2. …

#### Expected
…

#### Actual
…

#### Impact
…

#### Fix direction
…

#### Test plan
- [ ] …

#### Notes
<disclosure reminders, related CHANGELOG entries, duplicate risks>
```

After the dossier, emit a **triage table**:

| ID | Severity | Disclosure | Title | Component |
|---|---|---|---|---|

Use stable local IDs `PROBE-001` …. Sort critical → low; private security first within the same severity.

### Method hints by subsystem

**Writ pipeline.** Trace `parse_agent_commands` → capability check → handler → tool-result injection. Fence handling, truncated `/write`, and `/parallel` ordering are historically sharp edges.

**API server.** One thread per connection; fresh `Orchestrator` per request. Audit auth extraction once, then every handler's `tenant_id` threading into `TenantStore`. Confirm cancel via `InFlightRegistry` cannot UAF. Confirm `/v1/orchestrate` write interceptor + `file_max_bytes` cannot be skipped by alternate entrypoints (chat, A2A, events, schedules).

**Tenant store.** Every public getter should take `tenant_id` and filter in SQL, not in C++ after fetch. Watch admin paths and internal scheduler calls for accidental global listings.

**Sandbox.** Canonicalize then prefix-check workspace paths; ensure `docker` argv cannot inject opts; confirm lazy `ensure_container` is safe under concurrent first-use.

**TUI.** Degenerate geometry, shutdown ordering, and concurrent design/theme pointer swaps are prior crash classes — hunt generalizations, not the exact old bug.

**MCP/A2A.** Treat remote peers as hostile. Validate sizes, content-types, redirects, and that tenant scope is stamped on every persisted task/artifact.

### Done criteria

You are done when:

- P0 and P1 surfaces above have been explicitly considered (even if "no issue")
- Every emitted issue meets the quality bar
- Private security items are clearly separated and not filed publicly
- The triage table is ranked for maintainer action

If the tree is unexpectedly clean, say so and file only **test** issues that lock the invariants you verified — that is still high value.
