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
# per question per variant.  Pass `--rerank-fine-model` together with
# `--rerank-model` to exercise the two-stage reranker (cheap-then-strong
# pass, top 8 promoted to the fine pass with bigger excerpts).  This is
# the variant the headline numbers should be reported on — single-pass
# rerank is the floor, two-stage is the ceiling.
python3 query.py \
    --token "$ARBITER_TOKEN" \
    --manifest /tmp/manifest.json \
    --rerank-model claude-haiku-4-5 \
    --rerank-fine-model claude-sonnet-4-6 \
    --per-question-out /tmp/topk.json

# Step 2 — grade.py reads the dump, calls a generator model + a judge
# model on each question, and reports accuracy overall + by question
# type.  Needs ANTHROPIC_API_KEY in the environment (the judge calls
# bypass arbiter and go straight to api.anthropic.com).
#
# Use Sonnet as the generator for headline numbers; Haiku gives a cheap
# floor but leaves accuracy on the table on temporal-reasoning and
# multi-session synthesis where reasoning over the retrieved context is
# the bottleneck.  Top-K defaults to 10 — R@10 is ~2 points above R@5
# on rerank, and grade.py annotates each entry with session + timestamp
# so the wider window doesn't drown the answer in noise.
ANTHROPIC_API_KEY=sk-ant-... python3 grade.py \
    --manifest /tmp/manifest.json \
    --per-question-in /tmp/topk.json \
    --gen-model claude-sonnet-4-6 \
    --judge-model claude-sonnet-4-6 \
    --top-k 10 \
    --json-out /tmp/grade.json
```

### How the generator sees retrieved context

`grade.py` formats each top-K entry as:

```
[1] (session sess-3 · 2024-09-12 14:22) <title>
<content, clipped at 2000 chars>
```

Surfacing `session` and `created_at` is what closes the gap on
temporal-reasoning ("when did the user first mention X?") and
multi-session questions ("the user changed their mind between
sessions"), where retrieval is fine but the generator has no axes to
reason on without the metadata.  Entries that have been invalidated
(`valid_to` set) carry a `superseded at <ts>` marker so
knowledge-update questions can prefer the live fact.

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
