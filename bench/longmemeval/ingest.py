#!/usr/bin/env python3
"""
Ingest a LongMemEval-format dataset into a running arbiter --api instance.

Stdlib-only — no `pip install` step.  Iterates the dataset's sessions,
creates one conversation per session, writes one memory entry per turn,
and records the (question_id → expected_entry_ids, conversation_id)
mapping into a manifest file consumed by query.py.

See README.md for the dataset shape this expects.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any


def http_post(url: str, token: str, body: dict) -> dict:
    """POST JSON, return the parsed response.  Raises on non-2xx."""
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body_txt = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"POST {url} failed: {e.code} {e.reason}\n  body: {body_txt}"
        ) from None


def create_conversation(api: str, token: str, title: str) -> int:
    """Create a conversation, return its id."""
    # The orchestrator's conversation endpoint expects an agent_id; "index"
    # is the built-in master and always exists.
    resp = http_post(
        f"{api}/v1/conversations",
        token,
        {"title": title, "agent_id": "index"},
    )
    return int(resp["id"])


def create_entry(
    api: str,
    token: str,
    conversation_id: int,
    title: str,
    content: str,
    source: str,
) -> int:
    """Create a memory entry pinned to the given conversation, return id."""
    resp = http_post(
        f"{api}/v1/memory/entries",
        token,
        {
            "type": "context",
            "title": title[:200],   # storage-side cap
            "content": content[:64 * 1024],
            "source": source[:200],
            "tags": [],
            "conversation_id": conversation_id,
        },
    )
    return int(resp["id"])


def turn_title(session_id: str, turn_idx: int, role: str, content: str) -> str:
    """Short title that the FTS title-weight (10x) can latch onto.

    First ~140 chars of content prefixed by role; long enough to carry
    real signal, short enough to fit the title field's 200-char cap.
    """
    snippet = " ".join(content.split())[:140]
    return f"[{session_id}#{turn_idx}/{role}] {snippet}"


def ingest(api: str, token: str, dataset_path: str, manifest_path: str) -> None:
    print(f"loading dataset: {dataset_path}", file=sys.stderr)
    with open(dataset_path, encoding="utf-8") as f:
        data = json.load(f)

    questions = data.get("questions") or []
    if not questions:
        raise SystemExit("dataset has no `questions` array — wrong format?")

    # Across questions, sessions can be shared (the same haystack
    # session referenced by several test questions).  Cache by
    # (question_id, session_id) → conversation_id, and within that,
    # session → list-of-entry-ids so the manifest builds in one pass.
    #
    # We do NOT share conversations across questions on purpose: each
    # question gets its own ingest, so the conversation-scope variant
    # in query.py asks "find the answer inside *this* question's
    # conversation" — matching how an agent would have just been
    # talking with the user about the haystack.

    manifest_questions: list[dict[str, Any]] = []
    total_entries = 0

    for q_i, q in enumerate(questions):
        qid = str(q.get("question_id") or f"q-{q_i:04d}")
        question_text = q.get("question") or ""
        answer_session_ids = set(q.get("answer_session_ids") or [])
        haystack = q.get("haystack_sessions") or []

        if not question_text:
            print(f"  skip {qid}: no question text", file=sys.stderr)
            continue
        if not haystack:
            print(f"  skip {qid}: no haystack_sessions", file=sys.stderr)
            continue

        # One conversation per question's haystack.  Title is the
        # question text — useful when browsing the conversations
        # endpoint while debugging.
        conv_id = create_conversation(api, token, f"longmemeval/{qid}")

        expected_ids: list[int] = []
        for sess in haystack:
            sess_id = str(sess.get("session_id") or "")
            turns = sess.get("turns") or []
            for t_i, turn in enumerate(turns):
                role = str(turn.get("role") or "user")
                content = str(turn.get("content") or "")
                if not content.strip():
                    continue
                title = turn_title(sess_id, t_i, role, content)
                entry_id = create_entry(
                    api,
                    token,
                    conv_id,
                    title,
                    content,
                    f"longmemeval:{qid}:{sess_id}:{t_i}",
                )
                total_entries += 1
                # Coarse ground-truth: every entry from a session
                # listed in `answer_session_ids` counts as relevant
                # for this question.  See README → Metric definition.
                if sess_id in answer_session_ids:
                    expected_ids.append(entry_id)

        if not expected_ids:
            print(
                f"  warn {qid}: no expected entries (answer sessions "
                f"{answer_session_ids} not found in haystack)",
                file=sys.stderr,
            )

        manifest_questions.append(
            {
                "question_id": qid,
                "question": question_text,
                "conversation_id": conv_id,
                "expected_entry_ids": expected_ids,
            }
        )

        if (q_i + 1) % 25 == 0:
            print(
                f"  ingested {q_i + 1}/{len(questions)} questions "
                f"({total_entries} entries)",
                file=sys.stderr,
            )

    manifest = {
        "api": api,
        "questions": manifest_questions,
        "stats": {
            "total_questions": len(manifest_questions),
            "total_entries": total_entries,
        },
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(
        f"ingest complete: {len(manifest_questions)} questions, "
        f"{total_entries} entries → {manifest_path}",
        file=sys.stderr,
    )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--api",
        default="http://127.0.0.1:8080",
        help="arbiter --api base URL (default %(default)s)",
    )
    p.add_argument(
        "--token",
        required=True,
        help="tenant bearer token (atr_…) — get one from `arbiter --add-tenant`",
    )
    p.add_argument(
        "--dataset",
        required=True,
        help="path to the LongMemEval-format JSON file",
    )
    p.add_argument(
        "--manifest",
        required=True,
        help="path to write the ingest manifest (consumed by query.py)",
    )
    args = p.parse_args()

    try:
        ingest(args.api, args.token, args.dataset, args.manifest)
    except (urllib.error.URLError, RuntimeError) as e:
        print(f"ingest failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
