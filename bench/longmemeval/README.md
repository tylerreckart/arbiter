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
- The LongMemEval dataset.

## Running the benchmark

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

## Answer-grading accuracy (optional)

The R@K numbers above measure **retrieval quality**: did the right entry
land in the top K? Most other published memory frameworks (Mem0, Zep,
Letta) instead report **answer-grading accuracy**: an LLM-as-judge score
on a *generated answer* after retrieved entries are fed to a generator
model. That metric convolves retrieval quality with generator quality,
but it's the right number to publish for direct comparison against
those systems.

`grade.py` runs that two-stage pipeline on top of a `query.py` dump:

```bash
# Step 1 — query.py with --per-question-out to capture top-K entries
# (id+title+content) per question per variant.  This is just the
# normal query run with one extra flag.
python3 query.py \
    --token "$ARBITER_TOKEN" \
    --manifest /tmp/manifest.json \
    --rerank-model claude-haiku-4-5 \
    --per-question-out /tmp/topk.json

# Step 2 — grade.py reads the dump, calls a generator model + a judge
# model on each question, and reports accuracy overall + by question
# type.  Needs ANTHROPIC_API_KEY in the environment (the judge calls
# bypass arbiter and go straight to api.anthropic.com).
ANTHROPIC_API_KEY=sk-ant-... python3 grade.py \
    --manifest /tmp/manifest.json \
    --per-question-in /tmp/topk.json \
    --gen-model claude-haiku-4-5 \
    --judge-model claude-sonnet-4-6 \
    --top-k 5 \
    --json-out /tmp/grade.json
```

Useful flags while iterating:

- `--variants graduated,rerank` — grade only a subset of the variants
  in the dump (skip `bm25` if you only care about the realistic
  retrieval modes).
- `--limit 50` — grade only the first 50 questions per variant. Cuts
  cost from ~1000 LLM calls per full sweep to ~200; the headline
  number gets noisy but the per-question-type breakdown still tells
  you where the system is weak.

### Caveats on the resulting numbers

- **Judge dependence.** Different judge models disagree on the hard
  cases. `grade.py` defaults to `claude-sonnet-4-6` as the judge;
  papers publishing comparable numbers may use GPT-4o or GPT-4o-mini
  per the LongMemEval reference judge. To put arbiter's number into
  someone else's table, run with their judge.
- **Generator dependence.** Smarter generators rescue weak retrieval
  (they can hallucinate plausible answers) and inflate accuracy.
  Holding the generator constant across systems being compared is
  more important than choosing the "right" generator.
- **Cost.** A full run is `2 × variants × 500` LLM calls. With Haiku
  on both sides that's ~$0.30 / sweep; with Sonnet as judge, a few
  dollars. Use `--limit` for tighter loops.
