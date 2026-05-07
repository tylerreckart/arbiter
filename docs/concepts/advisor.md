# Advisor

Arbiter's advisor is a structurally separate model that watches an executor agent. Two operating modes are layered on the same configuration block:

- **Consult** (`mode: "consult"`) — opt-in. The executor invokes the advisor on demand via the `/advise <question>` slash command. The advisor replies in plain prose; the executor decides whether to follow the advice. This is the legacy behaviour.
- **Gate** (`mode: "gate"`) — structural. The runtime invokes the advisor automatically on the executor's *terminating turn* and parses a structured `CONTINUE` / `REDIRECT` / `HALT` signal. The executor cannot return a final result to its caller without the advisor's `CONTINUE`. This is the runtime-level supervision pattern described in [the philosophy doc](../../philosophy.md).

The two modes coexist. An agent in `gate` mode can still emit `/advise` for its own purposes; the gate runs in addition.

## Why a runtime-level gate

The gate exists below the agent's API surface — the executor cannot bypass it. That distinguishes structural supervision from advisory consultation: a self-supervising loop (asking the executor to be self-critical) catches a different failure mode than separating execution from judgment with the runtime owning the escalation path. The agent doing the work is usually the agent deciding whether the work is going well; the gate breaks that pattern.

## Configuration

The advisor block lives on a `Constitution` and accepts three shapes for back-compat:

```jsonc
// Modern, structured form — recommended.
"advisor": {
  "model": "claude-opus-4-7",
  "mode": "gate",
  "max_redirects": 2,
  "malformed_halts": true,
  "prompt": "...optional override of the gate's system prompt..."
}

// String shorthand — equivalent to {model: <s>, mode: "consult"}.
"advisor": "claude-opus-4-7"

// Legacy field — equivalent to {model: <s>, mode: "consult"}.
// Mirrored into advisor.model when no advisor block is present.
"advisor_model": "claude-opus-4-7"
```

If both `advisor` (object) and `advisor_model` are present, the structured `advisor` block wins. A stderr warning is logged.

| Field             | Type    | Default     | Notes |
|-------------------|---------|-------------|-------|
| `model`           | string  | (required when `mode != "off"`) | Higher-capability model used for both consults and gate decisions. |
| `mode`            | string  | `"consult"` | `"off"` / `"consult"` / `"gate"`. |
| `prompt`          | string  | built-in    | Override the gate's system prompt. Only consulted in `mode: "gate"`. |
| `max_redirects`   | int     | `2`         | Cap on how many `REDIRECT` signals the gate can issue per top-level turn before the runtime synthesises a `HALT`. |
| `malformed_halts` | bool    | `true`      | Whether an unparseable advisor reply is treated as `HALT` (fail-closed) or `CONTINUE` (fail-open). |

## Signal grammar (gate mode)

The gate's system prompt instructs the advisor to emit exactly one of three signals on its own line. Tag-based, case-insensitive on the signal token, body-text retains casing:

```
<signal>CONTINUE</signal>
```

```
<signal>REDIRECT</signal>
<guidance>concrete next step the executor should take</guidance>
```

```
<signal>HALT</signal>
<reason>why the executor must stop and the user must see this</reason>
```

The runtime parses the first `<signal>` block; surrounding prose is tolerated. Missing body on `REDIRECT` / `HALT` flags the reply as `malformed`. Unparseable input also flags `malformed`. Per `malformed_halts`, the runtime decides whether to fail-closed (`true` — promote to HALT) or fail-open (`false` — accept as CONTINUE).

## Runtime control flow

Where the gate fires inside the dispatch loop:

1. The executor's current turn produced no further tool calls (`cmds.empty()`) — i.e., it's about to terminate.
2. If `mode != "gate"`, terminate immediately (legacy behaviour).
3. Otherwise the runtime calls the gate with a structured input:
   - `original_task` — the user's original request (no truncation beyond the master's own delegation cap).
   - `terminating_text` — the executor's text for this turn only.
   - `tool_summary` — a one-line-per-call summary of the tools the executor used in the turn that produced these results: `- <name> args=<first 80 chars> result=<first 200 chars>`.

   The advisor sees nothing else — no prior turns, no full reasoning trace. Its system prompt is fixed (or `prompt`-overridden); its history is empty per call.

4. The runtime parses the reply into one of:
   - `CONTINUE` — the loop terminates and the response returns to the caller.
   - `REDIRECT` — the runtime synthesises a `[advisor redirect — synthetic user turn]` envelope wrapping the guidance and feeds it back to the executor as the next input. Counts against both `max_redirects` and the executor's per-turn budget (`kMaxTurns = 6`).
   - `HALT` — the runtime fires the `escalation` callback, then `stream_end_cb_(ok: false)`, and returns an `ApiResponse{ok: false, error_type: "advisor_halt", halt_reason: <reason>}`.

5. When `redirects_used >= max_redirects`, the runtime synthesises a `HALT` with reason `"advisor redirect budget exhausted"` rather than allowing infinite redirects.

## Escalation

A `HALT` surfaces via two distinct callbacks:

- `escalation_cb_(agent_id, stream_id, reason)` — fires first. SSE clients see this as an `escalation` event. Only fires at the originating depth; sub-agent halts bubble up via the parent's response, which then runs through the parent's gate (if any).
- `stream_end_cb_(agent_id, stream_id, /*ok=*/false)` — fires after, like any other failed turn.

CLI / TUI consumers can wire `set_escalation_callback` to print a banner; the API server emits an SSE `escalation` event. See [SSE events](sse-events.md#advisor-event-kinds).

## Per-depth behaviour

The gate runs at every delegation depth where the agent has `mode: "gate"` configured. A depth-1 sub-agent halt sets `error_type: "advisor_halt"` on its response, which the parent's dispatch loop sees as a failed `/agent` invocation. Only the originating depth fires the `escalation_cb_` so the user sees one banner per halt, not N banners for an N-deep delegation chain.

## Consult-only `/advise`

When `mode: "consult"` (or the legacy `advisor_model` is set), the executor can emit `/advise <question>`. This is unchanged from earlier releases:

- The advisor sees the question only — no history, no prior turns.
- The reply is plain prose, inlined into the executor's tool-result block.
- Per-turn budget: `kMaxAdvise = 2`. Exceeding it returns `SKIPPED: max 2 advisor consults per turn`.
- Output is rendered to verbose logs as an `advisor` SSE event with `kind: consult`.

The consult path doesn't constrain the executor — it's an affordance, not a gate. Use `mode: "gate"` when you want structural enforcement.

## See also

- [Philosophy](../../philosophy.md) — the structural-separation argument.
- [SSE events](sse-events.md) — `advisor` and `escalation` event payloads.
- [Agent data model](data-model.md) — where the advisor block lives.
- [`POST /v1/agents`](../agents/create.md) — create an agent with an advisor configured.
