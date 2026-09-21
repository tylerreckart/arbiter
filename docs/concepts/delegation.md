# Delegation policies

Constitutions can declare **runtime** limits on who an agent may spawn, how deep the pipeline may go, and how much delegated work may cost. The orchestrator enforces them at `/agent`, `/parallel`, and JIT ensure cover spawn. The model does not get a vote — a violation is a tool `ERR:`, the same posture as the global depth cap of 2.

This is not a new orchestrator. Conversational [`POST /v1/orchestrate`](../api/orchestrate.md) and reconcile observe/ensure stay on their existing loops; policy is a gate on spawn.

## Why a runtime gate

`capabilities` already decides whether an agent may emit `/agent` at all. That is a verb allowlist. Delegation policy is the *callee* allowlist (and budget) sitting next to it:

- Index can be told it may not send work to `forge`.
- A specialist can be told it may not re-delegate (max depth 1), or may not run as a child at all (max depth 0).
- A token or spend cap on the delegated subtree stops further spawn once it is exhausted.

Prompt text cannot do this. The runtime already refuses `depth >= 2` with `ERR: delegation depth limit reached (max 2 levels)` regardless of what the model wrote. Per-constitution policy uses that same fail-closed path.

Presence is the opposite: it is fail-open and **cannot halt**. Presence review does not consult this gate.

## Configuration

The `delegation` object lives on a `Constitution`. Absent / omitted fields keep today's behaviour (global depth 2, any catalog callee, no subtree budget). Stock `agents/*.json` starters do not set the block.

```jsonc
"delegation": {
  "max_depth": 1,
  "allowed_callees": ["scout", "vera"],
  "denied_callees": ["forge"],
  "max_subtree_tokens": 8000,
  "max_subtree_usd": 0.50
}
```

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `max_depth` | int 0..2 | `2` | Absolute pipeline depth this agent may spawn *to* (the child's depth). `0` = cannot spawn, and cannot itself run as a delegated or JIT worker. Must not exceed the global cap of 2. |
| `allowed_callees` | array\<string\> | `[]` | **Primary allowlist.** Non-empty: callee id must be in the list. Empty / omitted = no extra restriction beyond catalog existence. |
| `denied_callees` | array\<string\> | `[]` | Optional denylist applied *after* the allowlist. Empty / omitted = nobody extra is forbidden. Use this to forbid one id (`forge`) without listing the rest of the roster. |
| `max_subtree_tokens` | int ≥ 0 | unlimited | Cap on delegated work (children + their descendants) for the current top-level turn. Distinct from constitution `max_tokens` (response size). `0` = no spawn. Further spawn returns `ERR:` when spent ≥ cap; the in-flight child is not killed mid-turn. |
| `max_subtree_usd` | number ≥ 0 | unlimited | Same window as the token cap, using a coarse model-family USD estimate (Haiku / Sonnet / Opus / GPT / local=0; unknown hosted ≈ Sonnet). Not a billing ledger. |

Unknown keys, wrong types, `max_depth` outside 0..2, or invalid agent ids **throw** at `Constitution::from_json` (admit fail closed). A malformed policy file is skipped by `load_agents`; `POST /v1/agents` returns 400.

Both lists may be set: the callee must pass the allowlist (if any) and must not be on the denylist.

## What the runtime checks

On every spawn the caller is the agent that emitted `/agent` / `/parallel` (or `index` for JIT `run_ephemeral`). The child depth is `caller_depth + 1`.

1. Global cap: `depth >= 2` still returns the historical `ERR: delegation depth limit reached (max 2 levels)` / `ERR: /parallel cannot delegate past depth 2`.
2. Self-invoke and `index` as callee — unchanged.
3. Catalog existence — unchanged (`ERR: no agent '…'`).
4. **Caller `max_depth`:** child depth must be ≤ the caller's effective cap.
5. **Caller `allowed_callees` / `denied_callees`.**
6. **Callee `max_depth`:** the child must be allowed to *run* at that depth (a `max_depth: 0` agent cannot be a depth-1 worker).
7. **Caller subtree budget** (tokens and/or USD) against spend recorded for this top-level turn.

A child cannot re-delegate past *its* own `max_depth`. Index cannot send work to a forbidden callee. JIT ensure covers go through `run_ephemeral` at depth 1 with `index` as the caller, so a constitution on index that forbids `forge` (or a cover constitution with `max_depth: 0`) is honoured the same way as `/agent`. Nested `/agent` from a JIT clone uses that clone's constitution.

The gate runs **before** the child LLM call. Presence review does not.

## `ERR:` shapes

```
ERR: delegation depth limit reached (max 2 levels)
ERR: delegation depth limit reached (constitution max_depth 1)
ERR: callee 'forge' is not permitted by constitution.delegation.allowed_callees
ERR: callee 'forge' is forbidden by constitution.delegation.denied_callees
ERR: agent 'scout' cannot run at depth 1 (constitution max_depth 0)
ERR: delegation token budget exceeded (max_subtree_tokens 8000)
ERR: delegation spend budget exceeded (max_subtree_usd 0.5)
```

Do not retry the same spawn; the runtime will refuse it again. When a policy is present the system prompt also grows a `DELEGATION POLICY` block so the model sees the same numbers — that is hint, not enforcement.

## What this is not

- Not workflow recipes (ordered crews).
- Not advisor policy packs.
- Not plan-to-execution observability.
- Not a second fleet dashboard. A denied spawn is an `ERR:` tool result (`tool_call` `ok: false` on the existing stream).

## See also

- [Writ](writ.md) — `/agent` / `/parallel` verbs.
- [Reconcile](reconcile.md) — JIT ensure covers (`run_ephemeral`).
- [Fleet streaming](fleet-streaming.md) — depth 0 / 1 / 2.
- [Presence](presence.md) — fail-open; cannot halt.
- [`POST /v1/agents`](../api/agents/create.md) — constitution schema.
