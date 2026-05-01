# LongMemEval benchmark for arbiter

Measures recall@K on arbiter's memory-search surface using the published
[LongMemEval](https://github.com/xiaowu0162/LongMemEval) dataset (500
questions over multi-session conversations, ground-truth answer sessions
labeled).

The point of this benchmark is **retrieval quality**, not throughput.
"Did `/mem search <question>` return the entry containing the answer in
the top K?" That's the question agents are asking arbiter; that's what
we're measuring.

## What this measures

Three retrieval variants:

| Variant       | What gets exercised |
|---------------|---------------------|
| `bm25`        | `GET /v1/memory/entries?q=…` — single-pass FTS5 + Okapi-BM25 ranking, no scope hint. Pure lexical baseline. |
| `graduated`   | `GET /v1/memory/entries?q=…&conversation_id=<session>&graduated=true` — conversation-scoped first pass, tenant-wide fill. Locality-bias contribution. |
| `rerank`      | `GET /v1/memory/entries?q=…&conversation_id=<session>&graduated=true&rerank=<model>` — graduated retrieval, then the top-N candidates routed through `<model>` for a final reorder. Opt-in via `--rerank-model`; costs one LLM call per question. |

Each variant reports **R@1**, **R@5**, **R@10** — the fraction of
questions whose ground-truth entry appears in the top K of the result
set.

## Prerequisites

- A running `arbiter --api` instance (default `http://127.0.0.1:8080`).
- A tenant bearer token (from `arbiter --add-tenant <name>`).
- Python 3.9+ — stdlib only, no `pip install` step.
- The LongMemEval dataset, or the included `sample/dataset.json` for a
  smoke test.

## Quick start (smoke test on the synthetic sample)

```bash
# In one shell — start the API server with a fresh HOME so tests don't
# pollute your real ~/.arbiter.
export HOME=$(mktemp -d)
arbiter --add-tenant bench   # prints a bearer token starting with atr_…
arbiter --api --port 8080 &
APIPID=$!
export ARBITER_TOKEN=atr_<copy-from-add-tenant-output>

# In a second shell (or after backgrounding above) —
cd /path/to/arbiter/bench/longmemeval
python3 ingest.py \
    --token "$ARBITER_TOKEN" \
    --dataset sample/dataset.json \
    --manifest /tmp/manifest.json
python3 query.py \
    --token "$ARBITER_TOKEN" \
    --manifest /tmp/manifest.json

# Optional: also run the rerank variant.  Requires the API server to
# have an API key for the model's provider (ANTHROPIC_API_KEY or
# OPENAI_API_KEY in the environment that started arbiter --api).
python3 query.py \
    --token "$ARBITER_TOKEN" \
    --manifest /tmp/manifest.json \
    --rerank-model claude-haiku-4-5

kill $APIPID
```

Expected output is a small table per variant; the synthetic sample is
hand-crafted so `bm25` should land R@5 = 1.0 (every answer findable) and
`graduated` should match.

## Running against the real LongMemEval

1. Download the dataset:
   ```bash
   git clone https://github.com/xiaowu0162/LongMemEval.git
   # The dataset JSONs live under data/ — pick the split you want
   #   (longmemeval_s.json   — 500 questions, the headline benchmark)
   ```
2. Adapt the field names if your snapshot's shape differs from what
   `ingest.py` expects (see [Dataset format](#dataset-format)).
3. Ingest + query as above, pointing at the real JSON.

A full ingest of `longmemeval_s.json` writes ~50K-100K entries depending
on session length; allow a few minutes on a local SQLite + HTTP loop.

## Dataset format

`ingest.py` expects this JSON shape (consistent with the published
LongMemEval `_s` / `_m` / `_oracle` files; if your snapshot differs in
field names, edit the loader):

```json
{
  "questions": [
    {
      "question_id": "q-001",
      "question": "What was the user's preferred coffee origin?",
      "answer": "Honduras",
      "haystack_sessions": [
        {
          "session_id": "s-1",
          "turns": [
            {"role": "user",      "content": "I've been buying Honduran beans lately."},
            {"role": "assistant", "content": "Noted — Honduran origin preferred."}
          ]
        }
      ],
      "answer_session_ids": ["s-1"]
    }
  ]
}
```

Required:

- `questions[].question_id` — opaque string, used in the manifest.
- `questions[].question` — the FTS query.
- `questions[].haystack_sessions[].session_id` — opaque string.
- `questions[].haystack_sessions[].turns[].content` — the text that
  becomes one memory entry's content.
- `questions[].answer_session_ids` — at least one session id whose
  turns contain the answer. Every entry ingested from these sessions
  is treated as ground-truth-relevant for that question.

## Ingestion strategy

`ingest.py` runs in **naive mode**: one memory entry per turn, type
`context`, content = the turn's text. The entry is pinned to the
session's conversation via `conversation_id`.

This isn't how an agent would write entries in production — agents
would synthesize fewer, denser entries — but it isolates the
*retrieval* quality from the *writer's* classification choices, which
is the apples-to-apples comparison vs. MemPalace's verbatim-storage
approach.

A future `--ingest-mode=classify` option would route each turn through
an LLM to pick a type (`project` / `learning` / `reference` / etc) and
synthesize a denser entry. Not implemented today; feel free to
contribute.

## Metric definition

For each question:

- The set of **ground-truth entry ids** is every entry ingested from
  any session in `answer_session_ids`. (Coarse — the dataset labels
  sessions, not turns; some sessions contain ~tens of relevant turns.)
- A variant **hits** at K for a question if the result set's top K
  contains at least one ground-truth id.
- **R@K = hits / total_questions**.

Coarse labeling means the ceiling here is generally above true
"semantic precision" — a system retrieving any turn from the answer
session counts as a hit, even if that specific turn doesn't carry the
fact. This is consistent with how LongMemEval is reported elsewhere
(MemPalace: 96.6% R@5 raw with the same coarse labeling).

## Comparing to MemPalace's published numbers

| System                       | LongMemEval R@5 |
|------------------------------|-----------------|
| MemPalace raw (verbatim+BM25+vector)  | 96.6% |
| MemPalace hybrid v4 (held-out 450q)   | 98.4% |
| MemPalace + LLM rerank                | ≥99%  |

MemPalace's pipeline is BM25 + dense-vector + closet boosts + (optional)
reranker; arbiter's is BM25 + metadata boosts + graduated conversation
scope + (currently agent-only) advisor reranker. Different signal mix.
Don't read the gap between any two systems' raw numbers as definitive
without controlling for ingestion grain — the ground-truth label
density (per-session vs per-turn) materially shifts the ceiling.

The signal worth tracking over time is **arbiter vs. arbiter** — does
each change land a meaningful R@5 lift on the same dataset, same
ingestion pipeline.

## Open issues / follow-ups

- **`--ingest-mode=classify`** — turn-level type classification via an
  LLM, for a more agent-realistic comparison.
- **Per-question latency** — currently we report aggregate R@K only;
  a `--report=per-question` flag would dump full per-question results
  for debugging regressions.
- **Concurrency** — ingest is sequential. A `--concurrency=N` flag on
  the ingester would speed up ingestion of the full 500-question
  dataset.
- **Rerank billing integration** — the HTTP rerank path uses a
  per-request `ApiClient` with the operator's API keys but does not
  pre-flight against `ARBITER_BILLING_URL` or post `usage/record`.
  Agent-side `/mem search --rerank` does flow through the
  orchestrator's billing because it's part of an orchestrate request.
  If commercial deployments want HTTP-rerank to count against tenant
  budgets, we'd thread the billing client into the rerank handler.
