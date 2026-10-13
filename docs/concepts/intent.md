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

Closed `kind` values: `research`, `review`, `write`, `ops`, `frontend`, `backend`, `plan`, `market`, `social`, `multi`, `unknown`. They map onto the personal starter callsigns under `agents/*.json` (`scout`, `vera`, `quill`, `forge`, `loom`, `nexus`, `compass`, `beacon`, `echo`). `multi` means "needs decomposition"; seed slots may be filled.

LLM replies use a tagged grammar (same idea as the [advisor gate](advisor.md)):

```text
<intent>
<kind>research</kind>
<confidence>0.86</confidence>
<agent>scout</agent>
<brief>User wants a literature-style survey of X.</brief>
<todo>Find primary sources</todo>
<phase agent="scout" name="survey">…</phase>
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

Loop continuations and HTTP follow-ups that pass `original_query` skip classify+reroute entirely so the addressed agent stays sticky. `/loop` always pins `original_query`, so it never intent-reroutes. Requests with [`channel: "voice"`](voice.md) also skip reroute (the transcript is a spoken continuation, not a fresh specialist dispatch).

When reroute does fire, conversation history and compaction are mirrored requested → specialist before the turn and specialist → requested after, so the thread stays on the agent the caller addressed (API persist / next hydrate still work). The backup is turn-scoped RAII (thread-local), not a single slot on the shared Orchestrator — concurrent REPL panes can reroute at the same time without restoring the wrong specialist history.

## Where it runs

Depth **0** only, inside `Orchestrator::send` / `send_streaming`, before lesson/todo injection and `run_dispatch`. Only on **fresh** ingress (no `original_query`). Classification text is the user/event text.

- Short-circuit: rewrite the in-flight agent, prepend `[INTENT] …` plus `GOAL: <brief>`.
- Fall through to index: prepend `[INTENT] kind=… conf=… source=…` so existing master routing rules can use it.

[`POST /v1/events`](../api/events.md) uses the same path. Glob `event_types` still run first; if the event lands on `index`, intent may reroute. Explicit event `agent` bypasses both.

Plan/todo seeds appear on the Intent object, the SSE `intent` event, and [`POST /v1/intent`](../api/intent.md). Nothing is written to the todo store or `execute_plan` in this foundation.

Desired-end-state work (workspace + tests + rollback) is [`POST /v1/reconcile`](../api/reconcile.md) — see [Reconcile](reconcile.md). Classify still does not persist seeds.

## Observability

| Surface | What you see |
|---------|----------------|
| SSE `intent` | After `request_received`, before `stream_start`. See [SSE catalog](sse-events.md). |
| TUI | Applied: info-tinted `↗ research · heuristic` (callsign suffix when the agent id differs from the kind, e.g. `scout`). Classified but not applied: warning-tinted `· intent research · hint:scout`. Quiet when `kind` is empty/`unknown`. Distinct from `·` system activity and from `→ delegating:`. |
| Verbose API log | `--verbose` renders `intent` alongside `advisor` / `tool_call`. |

## Live logprob router

`ARBITER_INTENT_LIVE` defaults off. Set it to `luna` to replace the generative `<intent>` fallback on depth-0 ingress. Routing otherwise stays on the heuristic / hybrid / llm path above, including shadow mode.

1. **Heuristic.** `heuristic_classify` runs first. A target whose confidence is at least `intent.min_confidence` (0.8 on the master constitution) is used immediately. No model call.
2. **One Luna call.** Below that threshold the router calls `decide()` once. Options are joint labels (`scout|cheap`, `scout|strong`, …) so specialist and difficulty share a single output token. Difficulty is `P(strong) / (P(strong) + P(cheap))` and selects the cheap or strong executor model for that turn. A roster that does not fit in 20 labels is asked for the specialist only and uses `ARBITER_INTENT_DEFAULT_DIFFICULTY`.
3. **No second classifier.** Confidence under `ARBITER_INTENT_LUNA_MIN_CONFIDENCE` (default 0.55) still routes to the best guess (`luna_guess`). A timeout (`ARBITER_INTENT_LUNA_TIMEOUT_MS`), a transport error, or no observed label routes to `ARBITER_INTENT_DEFAULT_AGENT` (default `index`, which does not reroute). The advisor redirects a wrong guess. The timeout default is **400 ms and needs tuning** before live mode is useful. It is not a measured latency. A live check on 2026-10-13 was p50 1359 ms and p95 1564 ms over 4 calls; one-off curls took 4-5 s. At 400 ms nearly every Luna call would fall back.

The Luna call starts as soon as the request is in hand and overlaps lesson and todo loading for a speculative agent (the heuristic target, otherwise the default agent). If the final agent differs, that agent's context is loaded afterward. Successful decisions are cached (normalized text + roster, LRU, default 64 entries and 60s TTL). The option list and system prompt stay short and stable so the provider can cache the prefix; the request text is the suffix. The call uses the process OpenRouter connection pool.

Each decision logs `intent_route` with `tier`, `cache_hit`, `luna_ms`, and `total_ms`. `arbiter-decision-smoke` prints nearest-rank p50 and p95 over the samples it actually sent.

## Shadow logprob classifier

`ARBITER_INTENT_SHADOW=luna` runs a second classifier beside the one above and logs the result as `intent_shadow`. It does not change `kind`, `target_agent`, or routing.

The shadow backend is `LunaLogprobBackend`: options are single-letter labels, the request asks OpenRouter for `openai/gpt-6-luna` with reasoning effort `none`, one output token, and `logprobs` / `top_logprobs`. Each observed label's probability is `exp(logprob)` of that token. Labels the response did not include are unknown, not zero. Confidence is `(p - 1/n) / (1 - 1/n)` with `p` that raw probability and `n` the full option count, so one returned label is not confidence 1 unless the token's own probability is 1. `labels_observed` is how many option labels were actually in the logprobs.

As of 2026-10-13, OpenRouter's catalogue does not list `logprobs` for this id. A live call with reasoning off does return them, but only one `top_logprobs` entry regardless of `top_logprobs=20`. `provider.require_parameters=true` 404s, so the feature is unofficial. A rejection is logged and the heuristic/LLM decision stands.

`arbiter-decision-smoke` runs the same backend against `agents/*.json` when `OPENROUTER_API_KEY` is set. Without a key it exits without calling the model.

## See also

- [`POST /v1/intent`](../api/intent.md) — standalone classify (defaults to heuristic; opt into hybrid/llm)
- [`POST /v1/reconcile`](../api/reconcile.md) — desired end state + verification
- [`POST /v1/orchestrate`](../api/orchestrate.md)
- [Advisor](advisor.md) — post-turn supervision, not ingress routing
- [Architecture](architecture.md)
