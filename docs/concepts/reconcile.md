# Reconcile

Intent reconcile is the **desired-end-state** face of the [intent engine](intent.md). Callers declare what must be true of a workspace (`target_state` + `invariants`); Arbiter compiles that into a [state contract](#state-contract), observes the tree, requires **tests**, and optionally rolls back on failure.

This is distinct from `POST /v1/intent` / ingress classify-and-route. Classify answers "which specialist?". Reconcile answers "does this directory satisfy $S$ — with evidence?".

Phase A (this document) is the admit + contract + observe + verify + rollback facade. JIT spawn and $\Delta S$ waves land with [#208](https://github.com/tylerreckart/arbiter/issues/208); this surface compiles to the same contract so it will not become a second orchestrator.

## Why

Conversational `/v1/orchestrate` still works. Success there is prose in a `done` event. Reconcile makes success **structured**:

- Unknown named invariants are rejected at admit time (fail closed).
- Expressions over `target_state` that contradict the desired object (`amountUSD <= 15000` while `amountUSD` is `20000`) are rejected before any mutation.
- `satisfied` requires green verification when `require_tests` is true (the default). A workspace that merely *looks* right is not enough.
- `rollback_on_failure: true` snapshots the tree first and restores it if the run does not satisfy.

## Control loop

```text
target_state + invariants + workspace
        │
        ▼
 Admit (catalog + expr + workspace bind)
        │
        ▼
 Compile → state contract (clauses)
        │
        ▼
 Snapshot (if rollbackOnFailure)
        │
        ▼
 Observe S_current → ΔS
        │
        ├─ residual + mode=observe ──► failed (delta_unresolved) + brief
        │
        ├─ residual + mode=ensure without implementer ──► implement_required
        │
        ▼
 Verification (detected or explicit test command)
        │
        ├─ missing / red ──► failed (verification_missing | failed)
        │                    + restore snapshot if flagged
        ▼
 ΔS empty + tests green ──► satisfied
```

`mode` defaults to **`observe`**: classify the gap, run tests if the tree already looks complete, do not invent files. `ensure` is reserved for an implement hook (tests inject a stub; a later fleet shim writes through writs). HTTP `ensure` without that hook returns `implement_required` plus a compiled `brief` you can send to [`POST /v1/orchestrate`](../api/orchestrate.md).

## State contract

Compiled from the intent. Versioned JSON; clauses are deterministic checkers, not model self-claims.

| Checker | Meaning |
|---------|---------|
| `expr.holds` | Tiny comparison over `target_state` (`amountUSD <= 15000.00`). |
| `file.exists` | Named file under the workspace (`README.md`, `LICENSE`). |
| `workspace.mentions` | Cue words appear in-tree (`system`, `status`, 2FA tokens). |
| `verification.pass` | Required test runner exited 0. |

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

Same SSE fabric as orchestrate. Persist + replay via `request_status` / `request_events`. Typed row in `reconcile_runs`.

| Event | When |
|-------|------|
| `request_received` | First frame (agent=`reconcile`). |
| `reconcile.progress` | Admit / phase notes. |
| `reconcile.delta` | Clause residual + held. |
| `reconcile.verification` | Test command, exit, reason. |
| `reconcile.rollback` | Snapshot restored. |
| `reconcile.done` | Structured result (`status`, `contract`, `delta`, `evidence`). |
| `done` | Terminal aggregate (`ok` true only when `status=satisfied`). |

TUI / `--send` do not call this path yet. Use [`POST /v1/reconcile`](../api/reconcile.md) or [`@arbiter/sdk`](../../sdk/ts/README.md).

## See also

- [`POST /v1/reconcile`](../api/reconcile.md)
- [Intent](intent.md) — classify/route, not reconcile
- [Sandbox](sandbox.md)
- [Durable execution](durable-execution.md)
- ROADMAP Phase 5
