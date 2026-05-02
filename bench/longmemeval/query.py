#!/usr/bin/env python3
"""
Run the LongMemEval query workload against an arbiter --api instance and
report recall@K for each retrieval variant.

Reads the manifest produced by ingest.py.  For every question, runs
each variant against `GET /v1/memory/entries`, takes the top-K result
ids, and counts a hit when at least one expected id appears in the top
K.  Reports R@1, R@5, R@10 per variant.

Stdlib-only.  `from __future__ import annotations` makes all type
hints lazy strings so the modern generic + union syntax works on
Python 3.9.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any


KS = (1, 5, 10)
LIMIT = max(KS)


def http_get(url: str, token: str) -> dict:
    """GET JSON, return the parsed response.  Raises on non-2xx."""
    req = urllib.request.Request(
        url,
        method="GET",
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"GET {url} failed: {e.code} {e.reason}\n  body: {body}"
        ) from None


def search(
    api: str,
    token: str,
    query: str,
    *,
    conversation_id: int | None,
    graduated: bool,
    limit: int,
    rerank_model: str | None = None,
) -> list[dict[str, Any]]:
    """Run one search variant; return ranked entries with id/title/content.

    We keep title/content on the returned dicts so the answer-grading
    harness (grade.py) can format them as context without needing to
    re-query the API per entry.  `hit_at_k` extracts ids on demand.
    """
    params = {"q": query, "limit": str(limit)}
    if conversation_id is not None:
        params["conversation_id"] = str(conversation_id)
    if graduated:
        params["graduated"] = "true"
    if rerank_model:
        params["rerank"] = rerank_model
    url = f"{api}/v1/memory/entries?" + urllib.parse.urlencode(params)
    body = http_get(url, token)
    out: list[dict[str, Any]] = []
    for e in body.get("entries") or []:
        out.append(
            {
                "id": int(e["id"]),
                "title": e.get("title") or "",
                "content": e.get("content") or "",
            }
        )
    return out


def hit_at_k(ranked_ids: list[int], expected: set[int], k: int) -> bool:
    """At least one expected id present in the first k ranked ids."""
    return any(rid in expected for rid in ranked_ids[:k])


def run_variant(
    name: str,
    api: str,
    token: str,
    questions: list[dict[str, Any]],
    *,
    conversation_id: bool,
    graduated: bool,
    rerank_model: str | None = None,
    capture_top_k: bool = False,
) -> dict[str, Any]:
    """Run one variant across all questions, return aggregated metrics.

    When `capture_top_k` is set, the returned dict includes a
    `per_question` list of {question_id, top_k:[{id,title,content}]}
    so the answer-grading harness can format context without
    re-hitting the API.
    """
    counts = {k: 0 for k in KS}
    skipped = 0
    latencies_ms: list[float] = []
    per_question: list[dict[str, Any]] = []

    for q in questions:
        expected = set(q.get("expected_entry_ids") or [])
        if not expected:
            skipped += 1
            continue

        conv = q["conversation_id"] if conversation_id else None
        t0 = time.monotonic()
        try:
            ranked = search(
                api,
                token,
                q["question"],
                conversation_id=conv,
                graduated=graduated,
                limit=LIMIT,
                rerank_model=rerank_model,
            )
        except RuntimeError as e:
            print(f"  warn: query for {q['question_id']} failed: {e}",
                  file=sys.stderr)
            continue
        latencies_ms.append((time.monotonic() - t0) * 1000.0)

        ranked_ids = [r["id"] for r in ranked]
        for k in KS:
            if hit_at_k(ranked_ids, expected, k):
                counts[k] += 1

        if capture_top_k:
            per_question.append(
                {"question_id": q["question_id"], "top_k": ranked}
            )

    total = len(questions) - skipped
    metrics: dict[str, Any] = {
        "variant": name,
        "total_questions": total,
        "skipped_no_ground_truth": skipped,
    }
    for k in KS:
        metrics[f"r@{k}"] = (counts[k] / total) if total > 0 else 0.0

    # Quick latency sketch — not the headline metric, but useful for
    # spotting regressions.  Median + p95 only; full distributions are
    # out of scope for this harness.
    if latencies_ms:
        latencies_ms.sort()
        n = len(latencies_ms)
        metrics["latency_ms"] = {
            "median": latencies_ms[n // 2],
            "p95": latencies_ms[min(n - 1, int(n * 0.95))],
        }

    if capture_top_k:
        metrics["per_question"] = per_question
    return metrics


def fmt_pct(x: float) -> str:
    return f"{x * 100:.1f}%"


def print_report(results: list[dict[str, Any]]) -> None:
    print()
    print(f"{'variant':<14} {'N':>5} {'R@1':>8} {'R@5':>8} {'R@10':>8}"
          f" {'p50 (ms)':>10} {'p95 (ms)':>10}")
    print("-" * 70)
    for r in results:
        lat = r.get("latency_ms") or {}
        print(
            f"{r['variant']:<14} "
            f"{r['total_questions']:>5} "
            f"{fmt_pct(r['r@1']):>8} "
            f"{fmt_pct(r['r@5']):>8} "
            f"{fmt_pct(r['r@10']):>8} "
            f"{lat.get('median', 0):>10.1f} "
            f"{lat.get('p95', 0):>10.1f}"
        )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--api", default="http://127.0.0.1:8080",
                   help="arbiter --api base URL (default %(default)s)")
    p.add_argument("--token", required=True,
                   help="tenant bearer token (atr_…)")
    p.add_argument("--manifest", required=True,
                   help="manifest produced by ingest.py")
    p.add_argument("--rerank-model", default=None,
                   help="when set, runs a third `rerank` variant that "
                        "asks this model to reorder the FTS top-N. "
                        "Costs one LLM call per question; requires the "
                        "API server to have an API key for the model's "
                        "provider configured.")
    p.add_argument("--json-out", default=None,
                   help="optional path to dump full results as JSON")
    p.add_argument("--per-question-out", default=None,
                   help="optional path to dump per-question top-K entries "
                        "(id+title+content per ranked entry) for each "
                        "variant.  Consumed by grade.py.")
    args = p.parse_args()

    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    questions = manifest.get("questions") or []
    if not questions:
        print("manifest has no questions", file=sys.stderr)
        return 1

    print(f"running {len(questions)} questions against {args.api}",
          file=sys.stderr)

    capture = bool(args.per_question_out)
    results = [
        run_variant(
            "bm25",
            args.api, args.token, questions,
            conversation_id=False, graduated=False,
            capture_top_k=capture,
        ),
        run_variant(
            "graduated",
            args.api, args.token, questions,
            conversation_id=True, graduated=True,
            capture_top_k=capture,
        ),
    ]
    if args.rerank_model:
        results.append(run_variant(
            "rerank",
            args.api, args.token, questions,
            conversation_id=True, graduated=True,
            rerank_model=args.rerank_model,
            capture_top_k=capture,
        ))

    print_report(results)

    # Strip per-question payloads out of the aggregate report; they
    # belong in --per-question-out so the regular --json-out stays
    # small and easy to skim.
    aggregate = [
        {k: v for k, v in r.items() if k != "per_question"}
        for r in results
    ]
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(
                {"manifest": args.manifest, "results": aggregate},
                f, indent=2,
            )
        print(f"\nfull results → {args.json_out}", file=sys.stderr)

    if args.per_question_out:
        per_var = [
            {
                "variant": r["variant"],
                "questions": r.get("per_question") or [],
            }
            for r in results
        ]
        with open(args.per_question_out, "w", encoding="utf-8") as f:
            json.dump(
                {"manifest": args.manifest, "variants": per_var},
                f, indent=2,
            )
        print(
            f"per-question top-K → {args.per_question_out}",
            file=sys.stderr,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
