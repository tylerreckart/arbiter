# `POST /v1/reconcile`

**Auth:** tenant — _Status:_ beta

Compile a desired end state into a state contract, observe a bound workspace, require verification, and optionally roll back. Streams SSE progress and ends with `reconcile.done` plus a standard `done` frame.

Does **not** JIT-spawn specialists in this phase. `mode` defaults to `observe` (gap report + tests if the tree is already complete). See [Reconcile](../concepts/reconcile.md).

## Request

### Body

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `target_state` | object | yes | — | Desired end state. Domain fields are modeled in-workspace (not real-world side effects). |
| `invariants` | array\<string\> | no | `[]` | Expr (`amountUSD <= 15000.00`) or named catalog entries. Unknown names → 400. |
| `workspace` | object | yes | — | `{ "kind": "sandbox" }` or `{ "kind": "path", "root": "..." }`. |
| `verification` | object | no | `{ require_tests: true, command: "auto" }` | Test mandate. |
| `rollback_on_failure` | bool | no | `false` | Snapshot + restore on failure/cancel. |
| `mode` | string | no | `"observe"` | `observe` \| `ensure`. |
| `budgets` | object | no | — | `{ max_waves, max_wall_ms }` recorded on the contract. |

### Headers

| Header | Required | Purpose |
|--------|----------|---------|
| `Authorization` | yes | `Bearer <tenant token>`. |
| `Content-Type` | yes | `application/json`. |
| `Idempotency-Key` | no | Same 24h replay as orchestrate. |

```bash
curl -N http://127.0.0.1:8080/v1/reconcile \
  -H "Authorization: Bearer $ARBITER_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "target_state": {
      "system": "wire-transfer-portal",
      "account": "4820194",
      "amountUSD": 12500.00,
      "status": "SETTLED"
    },
    "invariants": [
      "amountUSD <= 15000.00",
      "require_two_factor_auth_prompt"
    ],
    "workspace": { "kind": "sandbox" },
    "verification": { "require_tests": true, "command": "auto" },
    "rollback_on_failure": true
  }'
```

`workspace.kind=path` requires `ARBITER_RECONCILE_ALLOW_PATH=1` (experimental trusted bind). Sandbox kind needs `ARBITER_SANDBOX_IMAGE`.

## Response

`Content-Type: text/event-stream`. Causal order:

1. `request_received`
2. `reconcile.progress`
3. `reconcile.delta`
4. `reconcile.verification`
5. `reconcile.rollback` (only when a snapshot was restored)
6. `reconcile.done` — structured result
7. `done` — `ok: true` iff `status` is `satisfied`

`reconcile.done` payload (also returned by `GET /v1/reconcile/:id` after completion):

```json
{
  "request_id": "a1b2c3d4e5f60718",
  "status": "satisfied",
  "reason": "ok",
  "contract": { "id": "reconcile", "version": 1, "clauses": [ ] },
  "delta": { "residual": [], "held": [ ], "empty": true },
  "verification": {
    "ran": true,
    "passed": true,
    "command": "make test",
    "exit_code": 0,
    "reason": "passed"
  },
  "files_changed": [],
  "rolled_back": false,
  "brief": "Reconcile the workspace so target_state is true…"
}
```

| `status` | Meaning |
|----------|---------|
| `satisfied` | $\Delta S$ empty and required tests passed. |
| `failed` | Residual clauses, missing/red tests, or `implement_required`. |
| `rolled_back` | Failed (or canceled) and the snapshot restore succeeded. |
| `canceled` | Kill-switch / `POST …/cancel` during the run. |

## Related routes

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/v1/reconcile/:id` | Typed result / in-flight status (tenant-scoped). |
| `GET` | `/v1/reconcile/:id/events` | Durable SSE replay — same handler as [`GET /v1/requests/:id/events`](requests/events.md). |
| `POST` | `/v1/reconcile/:id/cancel` | Sets the in-flight cancel flag (`POST /v1/requests/:id/cancel` also works). |

## Failure modes

| Status | When |
|--------|------|
| 400 | Missing `target_state` / `workspace`; unknown named invariant; contradictory expr; unsafe `verification.command`; path workspace without the allow env; sandbox kind without a usable sandbox. |
| 401 | Missing or invalid bearer; tenant disabled. |
| 405 | Method is not POST on the collection. |
| 429 | Tenant rate / concurrency limiter (same as orchestrate). |
| 500 | Persist or sandbox workspace create failed. |

Admit-time 400 bodies include `{ "error": "…", "code": "unknown_invariant" }` (or `contradictory_invariant`, `bad_workspace`, `unsafe_verification`, …).

## See also

- [Reconcile concept](../concepts/reconcile.md)
- [`POST /v1/intent`](intent.md) — classify/route only
- [`POST /v1/orchestrate`](orchestrate.md) — conversational implement path; feed it `brief` after an observe miss
- [`@arbiter/sdk`](../../sdk/ts/README.md)
