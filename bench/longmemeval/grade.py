#!/usr/bin/env python3
"""
Answer-grading accuracy on top of a query.py per-question dump.

For each question and each variant in --per-question-in:
  1. Format the variant's top-K retrieved entries as context.
  2. Prompt a generator model with (question, context) → candidate answer.
  3. Prompt a judge model with (question, gold answer, candidate answer)
     → "correct" / "incorrect".
Aggregate accuracy overall and broken down by `question_type`.

This is the metric most non-arbiter memory frameworks (Mem0, Zep,
Letta) publish on LongMemEval.  Numbers convolve retrieval quality
with generator quality, so they're not a substitute for the R@K
table query.py emits — they're a complement.

Stdlib-only.  Reads ANTHROPIC_API_KEY from the environment.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from typing import Any


ANTHROPIC_URL = "https://api.anthropic.com/v1/messages"
ANTHROPIC_VERSION = "2023-06-01"


def call_anthropic(
    api_key: str,
    model: str,
    user_text: str,
    *,
    max_tokens: int,
    system: str | None = None,
    timeout: float = 60.0,
) -> str:
    """One-shot Anthropic Messages call; return assistant text."""
    body: dict[str, Any] = {
        "model": model,
        "max_tokens": max_tokens,
        "messages": [{"role": "user", "content": user_text}],
    }
    if system:
        body["system"] = system
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        ANTHROPIC_URL,
        data=data,
        method="POST",
        headers={
            "x-api-key": api_key,
            "anthropic-version": ANTHROPIC_VERSION,
            "content-type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body_txt = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"anthropic call failed: {e.code} {e.reason}\n  body: {body_txt}"
        ) from None
    parts = payload.get("content") or []
    for p in parts:
        if p.get("type") == "text":
            return str(p.get("text") or "")
    return ""


def format_context(entries: list[dict[str, Any]], top_k: int) -> str:
    """Render top-K retrieved entries as a single context string.

    Plain markdown-ish format; no special tokens.  Truncate per-entry
    content to keep total prompt size predictable — long entries are
    already a sign the retriever pulled in noise, and clipping each
    to a few hundred chars keeps generator latency bounded across
    runs.
    """
    chunks: list[str] = []
    for i, e in enumerate(entries[:top_k], start=1):
        content = (e.get("content") or "").strip()
        if len(content) > 800:
            content = content[:800] + "…"
        title = (e.get("title") or "").strip()
        chunks.append(f"[{i}] {title}\n{content}")
    return "\n\n".join(chunks) if chunks else "(no memory available)"


GEN_SYSTEM = (
    "You are answering a question using retrieved memory entries from "
    "a long-running conversation. Use only the provided memory; if the "
    "memory does not contain the answer, say you don't know. Keep "
    "answers concise — a sentence or two at most."
)


def generate_answer(
    api_key: str, gen_model: str, question: str, context: str
) -> str:
    user = f"Memory:\n{context}\n\nQuestion: {question}\n\nAnswer:"
    return call_anthropic(
        api_key, gen_model, user, max_tokens=256, system=GEN_SYSTEM
    ).strip()


JUDGE_SYSTEM = (
    "You grade whether a candidate answer correctly answers a question, "
    "given a reference answer. The candidate is correct if it conveys "
    "the same factual content as the reference, even if phrased "
    "differently or briefer. It is incorrect if it is wrong, missing the "
    "key fact, or says it doesn't know. Respond with exactly one word: "
    "correct or incorrect."
)


def judge_answer(
    api_key: str,
    judge_model: str,
    question: str,
    gold: str,
    candidate: str,
) -> bool:
    user = (
        f"Question: {question}\n\n"
        f"Reference answer: {gold}\n\n"
        f"Candidate answer: {candidate}\n\n"
        f"Verdict (correct / incorrect):"
    )
    out = call_anthropic(
        api_key, judge_model, user, max_tokens=8, system=JUDGE_SYSTEM
    )
    # Look for the word "correct" not preceded by "in" — guards against
    # judges that hedge with "incorrect" or "not correct".  The system
    # prompt asks for one word, but judges occasionally elaborate.
    norm = out.strip().lower()
    if norm.startswith("incorrect") or norm.startswith("not correct"):
        return False
    return "correct" in norm and "incorrect" not in norm


def grade_variant(
    api_key: str,
    variant_name: str,
    variant_questions: list[dict[str, Any]],
    manifest_by_qid: dict[str, dict[str, Any]],
    *,
    gen_model: str,
    judge_model: str,
    top_k: int,
    limit_questions: int | None,
) -> dict[str, Any]:
    """Run gen+judge across one variant's per-question top-K dump."""
    correct = 0
    total = 0
    correct_by_type: dict[str, int] = {}
    total_by_type: dict[str, int] = {}
    failures: list[str] = []
    samples: list[dict[str, Any]] = []
    t_start = time.monotonic()

    iterable = variant_questions
    if limit_questions is not None:
        iterable = iterable[:limit_questions]

    for i, vq in enumerate(iterable):
        qid = vq.get("question_id") or ""
        manifest_q = manifest_by_qid.get(qid)
        if not manifest_q:
            continue
        question_text = manifest_q.get("question") or ""
        gold = manifest_q.get("answer") or ""
        qtype = manifest_q.get("question_type") or "unknown"
        if not gold:
            # No ground-truth answer — can't grade.  Skip without
            # counting against accuracy in either direction.
            continue

        context = format_context(vq.get("top_k") or [], top_k)
        try:
            candidate = generate_answer(
                api_key, gen_model, question_text, context
            )
            verdict = judge_answer(
                api_key, judge_model, question_text, gold, candidate
            )
        except RuntimeError as e:
            failures.append(f"{qid}: {e}")
            continue

        total += 1
        total_by_type[qtype] = total_by_type.get(qtype, 0) + 1
        if verdict:
            correct += 1
            correct_by_type[qtype] = correct_by_type.get(qtype, 0) + 1

        # Keep the first handful of samples in the report so a human
        # can sanity-check what the generator + judge are doing.
        if len(samples) < 5:
            samples.append(
                {
                    "question_id": qid,
                    "question_type": qtype,
                    "question": question_text,
                    "gold": gold,
                    "candidate": candidate,
                    "verdict": "correct" if verdict else "incorrect",
                }
            )

        if (i + 1) % 25 == 0:
            print(
                f"  [{variant_name}] graded {i + 1}/{len(iterable)} "
                f"({correct}/{total} correct so far)",
                file=sys.stderr,
            )

    elapsed = time.monotonic() - t_start
    metrics: dict[str, Any] = {
        "variant": variant_name,
        "gen_model": gen_model,
        "judge_model": judge_model,
        "top_k": top_k,
        "total_graded": total,
        "correct": correct,
        "accuracy": (correct / total) if total > 0 else 0.0,
        "elapsed_s": round(elapsed, 1),
        "failures": failures,
        "samples": samples,
    }
    if total_by_type:
        metrics["by_question_type"] = {
            qt: {
                "n": total_by_type[qt],
                "correct": correct_by_type.get(qt, 0),
                "accuracy": correct_by_type.get(qt, 0) / total_by_type[qt],
            }
            for qt in sorted(total_by_type)
        }
    return metrics


def fmt_pct(x: float) -> str:
    return f"{x * 100:.1f}%"


def print_report(results: list[dict[str, Any]]) -> None:
    print()
    print(f"{'variant':<12} {'N':>5} {'correct':>8} {'accuracy':>10}"
          f"  {'gen':<22} {'judge':<22}")
    print("-" * 90)
    for r in results:
        print(
            f"{r['variant']:<12} "
            f"{r['total_graded']:>5} "
            f"{r['correct']:>8} "
            f"{fmt_pct(r['accuracy']):>10}  "
            f"{r['gen_model']:<22} "
            f"{r['judge_model']:<22}"
        )
    # Per-question-type breakdown if the dataset shipped types.
    for r in results:
        bt = r.get("by_question_type") or {}
        if not bt:
            continue
        print(f"\n  {r['variant']} by question_type:")
        for qt, m in bt.items():
            print(f"    {qt:<32} {m['correct']:>3}/{m['n']:<3} "
                  f"({fmt_pct(m['accuracy'])})")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", required=True,
                   help="manifest produced by ingest.py (for gold "
                        "answers + question_type)")
    p.add_argument("--per-question-in", required=True,
                   help="per-question top-K dump produced by "
                        "query.py --per-question-out")
    p.add_argument("--gen-model", default="claude-haiku-4-5",
                   help="generator model (default %(default)s)")
    p.add_argument("--judge-model", default="claude-sonnet-4-6",
                   help="judge model (default %(default)s) — should "
                        "be at least as capable as the generator")
    p.add_argument("--top-k", type=int, default=5,
                   help="how many top-ranked entries to feed to the "
                        "generator as context (default %(default)s)")
    p.add_argument("--limit", type=int, default=None,
                   help="grade only the first N questions per variant "
                        "(useful for cost-bounded smoke tests)")
    p.add_argument("--variants", default=None,
                   help="comma-separated subset of variants to grade "
                        "(default: all in the dump). e.g. graduated,rerank")
    p.add_argument("--json-out", default=None,
                   help="optional path to dump full grading results")
    args = p.parse_args()

    api_key = os.environ.get("ANTHROPIC_API_KEY") or ""
    if not api_key:
        print(
            "ANTHROPIC_API_KEY not set — grade.py needs an Anthropic key "
            "to call the generator and judge models.",
            file=sys.stderr,
        )
        return 2

    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    manifest_by_qid = {
        q["question_id"]: q for q in (manifest.get("questions") or [])
    }
    if not manifest_by_qid:
        print("manifest has no questions", file=sys.stderr)
        return 1

    with open(args.per_question_in, encoding="utf-8") as f:
        dump = json.load(f)
    variants = dump.get("variants") or []
    if not variants:
        print("per-question dump has no variants — re-run query.py "
              "with --per-question-out", file=sys.stderr)
        return 1

    selected: set[str] | None = None
    if args.variants:
        selected = {v.strip() for v in args.variants.split(",") if v.strip()}

    results: list[dict[str, Any]] = []
    for v in variants:
        name = v.get("variant") or "?"
        if selected is not None and name not in selected:
            continue
        vqs = v.get("questions") or []
        if not vqs:
            print(f"  skip {name}: no questions in dump", file=sys.stderr)
            continue
        print(
            f"grading variant {name} "
            f"(gen={args.gen_model}, judge={args.judge_model}, "
            f"top_k={args.top_k}, n={len(vqs)})",
            file=sys.stderr,
        )
        results.append(
            grade_variant(
                api_key,
                name,
                vqs,
                manifest_by_qid,
                gen_model=args.gen_model,
                judge_model=args.judge_model,
                top_k=args.top_k,
                limit_questions=args.limit,
            )
        )

    if not results:
        print("no variants graded", file=sys.stderr)
        return 1

    print_report(results)

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "manifest": args.manifest,
                    "per_question_in": args.per_question_in,
                    "results": results,
                },
                f, indent=2,
            )
        print(f"\nfull grading results → {args.json_out}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
