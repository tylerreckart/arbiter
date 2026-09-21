# Reconcile

Intent reconcile is the **desired-end-state** face of the [intent engine](intent.md). Callers declare what must be true of a workspace (`target_state` + `invariants`); Arbiter compiles that into a [state contract](#state-contract), observes the tree, requires **tests**, and optionally rolls back on failure.

This is distinct from `POST /v1/intent` / ingress classify-and-route. Classify answers "which specialist?". Reconcile answers "does this directory satisfy $S$ — with evidence?".

This is **contract-driven reconcile with JIT residency** (ephemeral agents), not a new agent type. [Presence](presence.md) is the opposite lifetime: stay loaded and watch. Conversational [`POST /v1/orchestrate`](../api/orchestrate.md) is unchanged — this path sits beside it.

Phase A shipped admit + contract + observe + verify + rollback. Phase B ([#208](https://github.com/tylerreckart/arbiter/issues/208)) turns `mode=ensure` into $\Delta S$ waves of spawn → execute → re-observe → teardown.

## Why

Conversational `/v1/orchestrate` still works. Success there is prose in a `done` event. Reconcile makes success **structured**:

- Unknown named invariants are rejected at admit time (fail closed).
- Expressions over `target_state` that contradict the desired object (`amountUSD <= 15000` while `amountUSD` is `20000`) are rejected before any mutation.
- `satisfied` requires green verification when `require_tests` is true (the default). A workspace that merely *looks* right is not enough. Model self-claim and advisor `CONTINUE`/`REDIRECT`/`HALT` are **not** the success supervisor on this path.
- `rollback_on_failure: true` snapshots the tree first and restores it if the run does not satisfy.

## Control loop

```text
target_state + invariants + workspace  (+ optional agent_map / extra clauses)
        │
        ▼
 Admit (catalog + expr + workspace bind + budgets)
        │
        ▼
 Compile → state contract (clauses, agent_map applied)
        │
        ▼
 Snapshot (if rollbackOnFailure)
        │
        ▼
 Observe S_current → ΔS
        │
        ├─ residual + mode=observe ──► failed (delta_unresolved) + brief
        │
        ├─ residual + mode=ensure
        │      no implementer (stub or fleet invoker) ──► implement_required
        │      residual clause has no agent_map / clause.agent / fallback ──► unmapped_clauses
        │      else wave loop:
        │        while residual and under budgets and not canceled:
        │          resolve covering agents (≤ max_agents_per_wave)
        │          spawn ephemeral clones (never catalog history_)
        │          execute ensure work in the bound workspace
        │          teardown clones
        │          re-observe  (runtime proof — not a self-claim)
        │          checker miss → retry; cap → clause_retry_exhausted
        │
        ▼
 Verification (detected or explicit test command)
        │
        ├─ missing / red ──► failed (verification_missing | failed)
        │                    + restore snapshot if flagged
        ▼
 ΔS empty + tests green ──► satisfied
```

`mode` defaults to **`observe`**: classify the gap, run tests if the tree already looks complete, do not invent files and do not spawn agents. `ensure` runs the wave loop. Tests inject a per-wave `implement` stub (no live LLM). HTTP `ensure` wires an orchestrator ephemeral invoker (same clone semantics as `/parallel`) so real agent turns write into the bound workspace.

## JIT waves (ensure)

Each wave instantiates **at most** `min(N, max_agents_per_wave)` ephemeral agents for $N$ open implementation clauses, grouped by resolved agent id. After the clones return, they are torn down (drop clone `history_` / residency) **before** the next re-observe. Canonical catalog agent history is never mutated.

Agent resolution, in order:

1. Explicit `agent_map` for that clause id (empty string = unmapped — no fallback).
2. `StateClause.agent` (compile-time default or extra-clause field).
3. Capability fallback (`file.exists` / `workspace.mentions` / store checkers → `nexus`; `verification.pass` → `forge`).
4. Else `unmapped_clauses`. HTTP ensure that resolves to an agent missing from the tenant catalog fails `unknown_agent`.

There is no LLM clause→agent mapper on this path.

Cancel maps to the existing kill-switch (`POST /v1/reconcile/:id/cancel`). Mid-wave, clones are torn down before the run terminates `canceled`. `rollback_on_failure` still restores the snapshot.

## State contract

Compiled from the intent. Versioned JSON; clauses are deterministic checkers, not model self-claims.

| Checker | Meaning |
|---------|---------|
| `expr.holds` | Tiny comparison over `target_state` (`amountUSD <= 15000.00`). |
| `file.exists` | Named file under the workspace (`README.md`, `LICENSE`). |
| `workspace.mentions` | Cue words appear in-tree (`system`, `status`, 2FA tokens). |
| `verification.pass` | Required test runner exited 0. |
| `todo.completed` | Tenant todo with matching subject is `completed` (projection; fail closed if none). |
| `artifact.exists` | Tenant artifact with matching path exists (projection; fail closed if none). |
| `memory.active` | Active structured-memory row for `type:tag` (projection; fail closed if none). |

Unknown checkers fail closed (admit `unknown_checker` on extra clauses; observe residual `unknown checker` otherwise). No new fact store.

v1 named catalog (unknown names → HTTP 400):

- `require_two_factor_auth_prompt`
- `require_authentication`
- `require_readme`
- `require_license`

## Workspace

Explicit bind is required. No implicit process-cwd for the API.

| `workspace.kind` | Root | Notes |
|------------------|------|--------|
| `sandbox` | Per-tenant sandbox workspace (`ARBITER_SANDBOX_IMAGE`) | Default for `--api`. |
| `path` | Caller `root` | Experimental. Requires `ARBITER_RECONCILE_ALLOW_PATH=1`. |

## Verification

`verification.require_tests` defaults **true**. `command: "auto"` picks, in order: `npm test`, `python -m pytest`, `cargo test`, `go test ./...`, `ctest --output-on-failure`, `make test`. Override with a safe explicit command (no `$` `` ` `` `;|&<>()`). Undetectable auto + required tests → `verification_missing` — never `satisfied`.

## Rollback

When `rollback_on_failure` is true, Arbiter copies the workspace to `.arbiter-reconcile-snapshots/pre` (skipping that directory itself) before mutation. On terminal failure or cancel it restores the copy. Snapshot lives on the host bind for sandbox workspaces.

## Observability

Same SSE fabric as orchestrate. Persist + replay via `request_status` / `request_events`. Typed row in `reconcile_runs` (includes `mode`, `agent_map`, budgets).

| Event | When |
|-------|------|
| `request_received` | First frame (agent=`reconcile`). |
| `reconcile.progress` | Admit / phase notes. |
| `reconcile.delta` | Clause residual + held (once per observe, including each wave). |
| `agent.spawned` | Ephemeral clone created for a ΔS cover (`ensure` waves). |
| `agent.teardown` | Clone dropped after the wave's turns (always, including cancel). |
| `reconcile.verification` | Test command, exit, reason. |
| `reconcile.rollback` | Snapshot restored. |
| `reconcile.done` | Structured result (`status`, `contract`, `delta`, `evidence`, `waves`). |
| `done` | Terminal aggregate (`ok` true only when `status=satisfied`). |

TUI / `--send` do not call this path yet. Use [`POST /v1/reconcile`](../api/reconcile.md) or [`@arbiter/sdk`](../../sdk/ts/README.md).

## See also

- [`POST /v1/reconcile`](../api/reconcile.md)
- [Intent](intent.md) — classify/route, not reconcile
- [Presence](presence.md) — always-on residency; opposite of JIT
- [Sandbox](sandbox.md)
- [Durable execution](durable-execution.md)
- ROADMAP Phase 5
