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
    """POST JSON, return the parsed response.  Raises on non-2xx.

    `ensure_ascii=False` keeps non-ASCII (emoji, accents) on the wire
    as raw UTF-8.  The default `ensure_ascii=True` escapes non-BMP
    codepoints as surrogate pairs (`\\ud83d\\udcab` for `💫`), which
    arbiter's lightweight JSON parser doesn't fold back into a single
    codepoint — each surrogate gets stored as its 3-byte UTF-8 form,
    inflating the on-server byte count past our 200-byte title cap.
    Sending raw UTF-8 keeps client and server byte counts in sync.
    """
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
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


def clamp_utf8_bytes(s: str, max_bytes: int) -> str:
    """Clamp a string so its UTF-8 representation is ≤ max_bytes.

    The server measures the cap in bytes, not codepoints, so a naive
    `s[:200]` lets multi-byte characters (emoji, accents in real
    LongMemEval content) push past 200 bytes.  `errors='ignore'` on
    decode drops the trailing incomplete codepoint cleanly.
    """
    enc = s.encode("utf-8")
    if len(enc) <= max_bytes:
        return s
    return enc[:max_bytes].decode("utf-8", errors="ignore")


def create_entry(
    api: str,
    token: str,
    conversation_id: int,
    title: str,
    content: str,
    source: str,
) -> int:
    """Create a memory entry pinned to the given conversation, return id.

    Hard-clamps title (≤ 200 B), content (≤ 64 KiB), source (≤ 200 B) at
    the boundary so the storage layer's caps never reject a turn.
    """
    payload = {
        "type": "context",
        "title": clamp_utf8_bytes(title, 200),
        "content": clamp_utf8_bytes(content, 64 * 1024),
        "source": clamp_utf8_bytes(source, 200),
        "tags": [],
        "conversation_id": conversation_id,
    }
    try:
        resp = http_post(f"{api}/v1/memory/entries", token, payload)
    except RuntimeError:
        # Surface the exact payload sizes so cap mismatches between
        # the client clamp and the server-side validator are obvious.
        title_b = len(payload["title"].encode("utf-8"))
        source_b = len(payload["source"].encode("utf-8"))
        content_b = len(payload["content"].encode("utf-8"))
        print(
            f"[create_entry] failed payload: "
            f"title={title_b}B source={source_b}B content={content_b}B "
            f"title-prefix={payload['title'][:80]!r}",
            file=sys.stderr,
        )
        raise
    return int(resp["id"])


def turn_title(session_id: str, turn_idx: int, role: str, content: str) -> str:
    """Short title that the FTS title-weight (10x) can latch onto.

    Long enough to carry real signal in the highest-weighted FTS field,
    short enough to fit the storage layer's 200-char cap.  Includes a
    role/session marker so collisions on identical openings stay
    distinguishable.  Hard-clamped to 200 since some LongMemEval
    session ids run long enough to bust a naive 140-char snippet
    budget when the prefix is included.
    """
    snippet = " ".join(content.split())
    title = f"[{session_id}#{turn_idx}/{role}] {snippet}"
    if len(title) > 200:
        title = title[:200]
    return title


def ingest(api: str, token: str, dataset_path: str, manifest_path: str) -> None:
    print(f"loading dataset: {dataset_path}", file=sys.stderr)
    with open(dataset_path, encoding="utf-8") as f:
        data = json.load(f)

    # The published LongMemEval JSONs are a list of question objects.
    # Accept the older `{"questions": [...]}` wrapper too for synthetic
    # fixtures that follow that shape (our smoke fixture does).
    if isinstance(data, list):
        questions = data
    elif isinstance(data, dict):
        questions = data.get("questions") or []
    else:
        raise SystemExit("dataset is neither a list nor an object with `questions`")
    if not questions:
        raise SystemExit("dataset has no questions — wrong format?")

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
        # The real dataset stores session ids parallel to the sessions
        # list; the synthetic fixture nests them inside session objects.
        # Accept both: the parallel form takes precedence when present.
        haystack_ids = q.get("haystack_session_ids") or []

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

        # Collect (entry_id, sess_id, has_answer) per ingested turn,
        # then resolve ground truth at the end.  This keeps the HTTP
        # loop straightforward; the relevance decision is a quick
        # in-memory pass once everything's written.
        ingested: list[tuple[int, str, bool]] = []

        for s_i, sess in enumerate(haystack):
            # Two shapes accepted:
            #   • list of turn dicts        (LongMemEval real format)
            #   • {"session_id":..., "turns":[...]}   (synthetic fixture)
            if isinstance(sess, list):
                turns = sess
                sess_id = (str(haystack_ids[s_i])
                           if s_i < len(haystack_ids) else f"sess-{s_i}")
            elif isinstance(sess, dict):
                turns = sess.get("turns") or []
                sess_id = str(sess.get("session_id") or
                              (haystack_ids[s_i] if s_i < len(haystack_ids)
                               else f"sess-{s_i}"))
            else:
                continue

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
                ingested.append(
                    (entry_id, sess_id, bool(turn.get("has_answer")))
                )

        # Ground truth: prefer turn-level `has_answer` flags when any
        # are set; otherwise fall back to coarse session-level
        # relevance via `answer_session_ids`.  This is the right
        # fall-through for datasets where some questions have flagged
        # turns and others only have session-level labels.
        any_flagged = any(flag for _, _, flag in ingested)
        if any_flagged:
            expected_ids = [eid for eid, _, flag in ingested if flag]
        else:
            expected_ids = [eid for eid, sid, _ in ingested
                            if sid in answer_session_ids]

        if not expected_ids:
            print(
                f"  warn {qid}: no expected entries (answer sessions "
                f"{answer_session_ids} not found in haystack, no "
                f"turn-level has_answer flags either)",
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
