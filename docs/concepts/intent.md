# Intent

Arbiter's intent engine sits **in front of** the orchestration loop. It classifies a user utterance or ingested event, optionally short-circuits the master (`index`) to a specialist, and may attach plan/todo **seeds**. It does not replace writ dispatch, does not persist todos, and does not execute plans.

This is distinct from [`memory.intent_routing`](structured-memory.md#intent-routing-heuristic-free), which only boosts `/mem search` type scores. Same word, different layer.

## Why a runtime classifier

Today's master routes by constitution `goal` / `capabilities` inside a prompt. That still works. Intent adds a cheap, testable first pass:

- High-confidence heuristic match → skip the index hop and send the specialist a brief.
- Uncertain → one history-less advisor-model call (hybrid), or fall through to index.
- Always fail open: parse/transport/missing-model errors leave routing unchanged.

The executor still speaks [writs](writ.md). Intent never becomes a writ.

## Hybrid classify

```text
explicit non-index agent  → source=explicit (no reroute)
heuristic unique match    → source=heuristic, maybe reroute
else if mode=hybrid|llm   → one LLM call, tagged <intent> reply
else                      → fall through to index
```

Closed `kind` values: `research`, `review`, `write`, `ops`, `frontend`, `backend`, `plan`, `market`, `social`, `multi`, `unknown`. They align with the starter agents under `agents/*.json`. `multi` means "needs decomposition"; seed slots may be filled.

LLM replies use a tagged grammar (same idea as the [advisor gate](advisor.md)):

```text
<intent>
<kind>research</kind>
<confidence>0.86</confidence>
<agent>research</agent>
<brief>User wants a literature-style survey of X.</brief>
<todo>Find primary sources</todo>
<phase agent="research" name="survey">…</phase>
</intent>
```

Unknown `<agent>` ids vs the loaded roster are dropped. Malformed replies clear the target and continue.

## Configuration

The `intent` block lives on a `Constitution`. File-backed agents default **off**. [`master_constitution()`](../../src/constitution.cpp) enables **hybrid** on `index`.

```jsonc
"intent": {
  "mode": "hybrid",          // off | heuristic | hybrid | llm
  "min_confidence": 0.8,
  "apply_routing": true,     // false = classify + SSE only, do not change agent
  "model": ""                // empty → advisor.model
}
```

| Field | Default (master) | Default (file agents) |
|-------|------------------|------------------------|
| `mode` | `"hybrid"` | `"off"` |
| `min_confidence` | `0.8` | `0.8` |
| `apply_routing` | `true` | `true` |
| `model` | `""` | `""` |

Reroute only happens when all of these hold: **fresh ingress** (`original_query` omitted), ingress agent is `index`, `apply_routing` is true, confidence ≥ threshold, and `target_agent` is loaded. Explicit `agent` on `/v1/orchestrate` or `/v1/events` always wins.

Loop continuations and HTTP follow-ups that pass `original_query` skip classify+reroute entirely so the addressed agent stays sticky. `/loop` always pins `original_query`, so it never intent-reroutes.

When reroute does fire, conversation history and compaction are mirrored requested → specialist before the turn and specialist → requested after, so the thread stays on the agent the caller addressed (API persist / next hydrate still work).

## Where it runs

Depth **0** only, inside `Orchestrator::send` / `send_streaming`, before lesson/todo injection and `run_dispatch`. Only on **fresh** ingress (no `original_query`). Classification text is the user/event text.

- Short-circuit: rewrite the in-flight agent, prepend `[INTENT] …` plus `GOAL: <brief>`.
- Fall through to index: prepend `[INTENT] kind=… conf=… source=…` so existing master routing rules can use it.

[`POST /v1/events`](../api/events.md) uses the same path. Glob `event_types` still run first; if the event lands on `index`, intent may reroute. Explicit event `agent` bypasses both.

Plan/todo seeds appear on the Intent object, the SSE `intent` event, and [`POST /v1/intent`](../api/intent.md). Nothing is written to the todo store or `execute_plan` in this foundation.

## Observability

| Surface | What you see |
|---------|----------------|
| SSE `intent` | After `request_received`, before `stream_start`. See [SSE catalog](sse-events.md). |
| TUI | Info-tinted `· [intent research heuristic → research]` (warning tint when classified but not applied). Same `·` chrome as advisor; color is the differentiator. |
| Verbose API log | `--verbose` renders `intent` alongside `advisor` / `tool_call`. |

## See also

- [`POST /v1/intent`](../api/intent.md) — standalone classify
- [`POST /v1/orchestrate`](../api/orchestrate.md)
- [Advisor](advisor.md) — post-turn supervision, not ingress routing
- [Architecture](architecture.md)
