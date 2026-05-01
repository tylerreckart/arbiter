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
