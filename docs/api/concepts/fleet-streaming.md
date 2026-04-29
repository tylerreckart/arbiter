# Fleet streaming

Every agent turn — master, delegated, or parallel child — is assigned a monotonically-increasing **`stream_id`** (the master is `0`; children increment from `1`). Every SSE event carries its stream id, so when the master calls `/parallel` and three sub-agents run concurrently, their `text` deltas interleave on one HTTP connection and the consumer routes each event into the right UI slot.

## Consumer routing rule

Open a UI slot on `stream_start`, route every subsequent event with matching `stream_id` to that slot, close the slot on `stream_end`. Read `depth` to decide slot layout:

| `depth` | Meaning |
|---------|---------|
| 0 | The master orchestrator turn. |
| 1 | A delegated sub-agent (via `/agent` or `/parallel`). |
| 2 | A sub-sub-agent (delegation by a depth-1 agent). |

The depth cap is 2; attempts to delegate past depth 2 surface to the requesting agent as an `ERR:` tool result.

## When `/parallel` is in play

The master emits a `/parallel … /endparallel` block with N `/agent` lines inside. Arbiter:

1. Spawns one thread per child at `depth + 1`, each with a fresh `stream_id`.
2. All children start concurrently — `stream_start` for each fires near-simultaneously, in thread-start order (not necessarily input order).
3. Each child streams independently through its lifecycle: `text`, `tool_call`, `token_usage`, `sub_agent_response`, `stream_end`.
4. Arbiter joins every child thread, aggregates the results into one tool-result block, and hands it back to the master.
5. The master resumes on its original `stream_id`, emits its synthesis turn, and closes.

### Example event sequence (master + 3-way fan-out)

```
request_received
stream_start      stream_id=0 depth=0 agent="index"
text              stream_id=0 agent="index" delta="Plan: fan out three..."
text              stream_id=0 agent="index" delta="/endparallel\n"
stream_start      stream_id=1 depth=1 agent="researcher_a"
stream_start      stream_id=2 depth=1 agent="researcher_b"
stream_start      stream_id=3 depth=1 agent="researcher_c"
agent_start       stream_id=3 agent="researcher_c"
agent_start       stream_id=2 agent="researcher_b"
agent_start       stream_id=1 agent="researcher_a"
text              stream_id=3 agent="researcher_c" delta="RESULT: Rome..."
text              stream_id=2 agent="researcher_b" delta="RESULT: Berlin..."
text              stream_id=1 agent="researcher_a" delta="RESULT: Paris..."
sub_agent_response stream_id=3 agent="researcher_c"
token_usage       stream_id=3 agent="researcher_c" billed_micro_cents=2549
stream_end        stream_id=3 ok=true
sub_agent_response stream_id=2 agent="researcher_b"
token_usage       stream_id=2 agent="researcher_b" billed_micro_cents=2711
stream_end        stream_id=2 ok=true
sub_agent_response stream_id=1 agent="researcher_a"
token_usage       stream_id=1 agent="researcher_a" billed_micro_cents=2525
stream_end        stream_id=1 ok=true
tool_call         stream_id=0 tool="parallel" ok=true
token_usage       stream_id=0 agent="index" billed_micro_cents=4204
text              stream_id=0 agent="index" delta="Paris, Berlin, and Rome..."
stream_end        stream_id=0 ok=true
done              ok=true billed_micro_cents=16043
```

## Parallel safety rails

- **Same `agent_id` reused in `/parallel` is allowed.** Each child runs on an ephemeral `Agent` instance built from the canonical agent's `Constitution`, so siblings have independent `history_` vectors and don't race. (This was a constraint pre-2026-04 but is now lifted.)
- **Depth cap.** A depth-2 turn cannot `/parallel`; attempts return an ERR tool result.
- **Each parallel child gets its own dedup cache.** Sibling threads fetching the same URL both fetch — accept the duplicate over a `std::map` data race.
- **SSE writes are serialized.** A shared mutex on the wire-writer means events interleave cleanly even when N threads emit at once.

## Master text gating

The master orchestrator's text deltas during a delegation iteration are **suppressed** — only a one-line `→ delegating: …` status line is emitted, and the master's prose lands only in the final synthesis iteration (after all children complete). This keeps "the orchestrator is answering before its sub-agents finished" out of the live UI. Sub-agent text continues to stream live in their own slots.

## See also

- [SSE event catalog](sse-events.md)
- [`POST /v1/orchestrate`](../orchestrate.md)
