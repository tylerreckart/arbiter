#!/usr/bin/env python3
"""Arbiter voice bridge — generic whisper.cpp + Piper hardware bridge.

Accepts a WAV upload from any device, runs local STT, routes to Arbiter,
synthesises the response with Piper, and returns a WAV.  Designed to sit
between any audio-capable microcontroller and a locally-running Arbiter API.

Pipeline per request:
  1. Auth check, body-size cap, per-IP rate limit.
  2. Transcribe uploaded WAV with whisper.cpp.
  3. Classify transcript: reset command, local tier, or cloud tier.
  4. Local tier  → stateless /v1/orchestrate (fast, no memory).
  5. Cloud tier  → /v1/conversations/:id/messages (persistent memory).
  6. Synthesise each sentence with Piper as text arrives (overlaps generation).
  7. Concatenate PCM → WAV header → return with Content-Length.

Endpoints:
  POST /v1/utterance   WAV upload → WAV response  (main device endpoint)
  POST /v1/transcribe  WAV upload → JSON {"transcript": "..."}  (debug/test)
  GET  /health         200 ok\\n

Required environment variables:
  ARBITER_TOKEN               Arbiter bearer token (atr_...)
  WHISPER_MODEL               path to whisper.cpp ggml model file
  PIPER_MODEL                 path to Piper .onnx voice model file

Optional environment variables:
  ARBITER_URL                 default http://127.0.0.1:8080
  ARBITER_LOCAL_AGENT         default local   (Ollama-backed fast agent)
  ARBITER_CLOUD_AGENT         default index   (cloud model, owns the conversation)
  WHISPER_BIN                 default whisper-cli
  PIPER_BIN                   default piper
  PIPER_SAMPLE_RATE           default 16000   (must match the Piper model's output rate)
  BRIDGE_API_KEY              if set, requests must carry Authorization: Bearer <key>
                              if unset, the bridge warns and binds to loopback only
  BRIDGE_MAX_BYTES            max upload bytes, default 524288 (512 KB)
  BRIDGE_RATE_LIMIT           max requests per IP per 60 s, default 20
  BRIDGE_CONVERSATION_FILE    path to persist conversation state across restarts
                              (e.g. /etc/3bo/conversation.json)
                              if unset, cloud turns are stateless (no memory)

Memory model:
  Simple/local queries (arithmetic, time, greetings) are routed stateless to
  the local Ollama agent — they do not benefit from history and the round-trip
  would add latency.

  Complex/cloud queries are sent through a persistent Arbiter conversation so
  the agent remembers prior turns.  The conversation_id is saved to
  BRIDGE_CONVERSATION_FILE and survives bridge restarts.

  Say "forget everything", "start fresh", "reset", or similar to start a new
  conversation.  The bridge creates a fresh conversation and acknowledges.

Arbiter agent setup (one-time):
  arbiter --api &

  # Fast local agent (Ollama must be running)
  curl -sX POST http://127.0.0.1:8080/v1/agents \\
    -H "Authorization: Bearer $ARBITER_TOKEN" \\
    -H "Content-Type: application/json" \\
    -d '{"id":"local","model":"ollama/gemma3:4b","max_tokens":256,
         "goal":"Answer simple questions concisely in one or two sentences."}'
"""

from __future__ import annotations

import argparse
import http.client
import json
import logging
import math
import os
import re
import struct
import subprocess
import tempfile
import threading
import time
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Iterator
from urllib.parse import urlparse


# ──────────────────────────────────────────────────────────────────────────────
# Configuration
# ──────────────────────────────────────────────────────────────────────────────

def _require(name: str) -> str:
    v = os.environ.get(name, "").strip()
    if not v:
        raise SystemExit(f"required env var {name!r} is not set")
    return v


def _opt(name: str, fallback: str) -> str:
    return os.environ.get(name, fallback).strip() or fallback


ARBITER_TOKEN = _require("ARBITER_TOKEN")
WHISPER_MODEL = _require("WHISPER_MODEL")
PIPER_MODEL   = _require("PIPER_MODEL")

ARBITER_URL        = _opt("ARBITER_URL",               "http://127.0.0.1:8080")
ARBITER_LOCAL      = _opt("ARBITER_LOCAL_AGENT",       "local")
ARBITER_CLOUD      = _opt("ARBITER_CLOUD_AGENT",       "index")
WHISPER_BIN        = _opt("WHISPER_BIN",               "whisper-cli")
PIPER_BIN          = _opt("PIPER_BIN",                 "piper")
PIPER_SAMPLE_RATE  = int(_opt("PIPER_SAMPLE_RATE",     "16000"))
BRIDGE_API_KEY     = os.environ.get("BRIDGE_API_KEY",  "").strip()
BRIDGE_MAX_BYTES   = int(_opt("BRIDGE_MAX_BYTES",      "524288"))
BRIDGE_RATE_LIMIT  = int(_opt("BRIDGE_RATE_LIMIT",     "20"))
BRIDGE_CONV_FILE   = os.environ.get("BRIDGE_CONVERSATION_FILE", "").strip()

_parsed      = urlparse(ARBITER_URL)
ARBITER_HOST = _parsed.hostname or "127.0.0.1"
ARBITER_PORT = _parsed.port or 8080

_RATE_WINDOW = 60

_rate_lock: threading.Lock          = threading.Lock()
_rate_table: dict[str, list[float]] = {}

log = logging.getLogger("voice-bridge")


# ──────────────────────────────────────────────────────────────────────────────
# WAV helpers
# ──────────────────────────────────────────────────────────────────────────────

def make_wav(pcm: bytes, sample_rate: int = PIPER_SAMPLE_RATE) -> bytes:
    """Prepend a 44-byte WAV header to raw signed-16-bit mono PCM."""
    channels    = 1
    bits        = 16
    byte_rate   = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(pcm), b"WAVE",
        b"fmt ", 16, 1, channels, sample_rate, byte_rate, block_align, bits,
        b"data", len(pcm),
    )
    return header + pcm


def _fallback_tone(duration_s: float = 0.35) -> bytes:
    """400 Hz sine burst — returned when Piper is unavailable."""
    n   = int(PIPER_SAMPLE_RATE * duration_s)
    buf = bytearray()
    for i in range(n):
        env = min(1.0, i / 800, (n - i) / 800)
        buf.extend(struct.pack("<h", int(3000 * env * math.sin(2 * math.pi * 440 * i / PIPER_SAMPLE_RATE))))
    return bytes(buf)


# ──────────────────────────────────────────────────────────────────────────────
# TTS — Piper
# ──────────────────────────────────────────────────────────────────────────────

def tts_sentence(text: str) -> bytes:
    """Synthesise one sentence with Piper; return raw PCM or b'' on failure."""
    text = text.strip()
    if not text:
        return b""
    try:
        r = subprocess.run(
            [PIPER_BIN, "--model", PIPER_MODEL, "--output_raw"],
            input=text.encode(),
            capture_output=True,
            timeout=30,
        )
        if r.returncode != 0:
            log.warning("piper exit=%d stderr=%s", r.returncode, r.stderr[:120])
            return b""
        return r.stdout
    except FileNotFoundError:
        log.error("piper not found at %r", PIPER_BIN)
        return b""
    except subprocess.TimeoutExpired:
        log.error("piper timed out synthesising %d chars", len(text))
        return b""


def make_error_wav(message: str) -> bytes:
    """Return a spoken error WAV, or a short tone when Piper is unavailable."""
    pcm = tts_sentence(message)
    return make_wav(pcm if pcm else _fallback_tone())


# ──────────────────────────────────────────────────────────────────────────────
# STT — whisper.cpp
# ──────────────────────────────────────────────────────────────────────────────

def transcribe(wav_path: str) -> str:
    """Return transcript text, or '' on silence/failure."""
    txt_path = wav_path + ".txt"
    try:
        r = subprocess.run(
            [WHISPER_BIN, "-m", WHISPER_MODEL, "-f", wav_path,
             "-otxt", "-nt", "-l", "en"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if r.returncode != 0:
            log.warning("whisper exit=%d stderr=%s", r.returncode, r.stderr[:200])
            return ""

        if os.path.exists(txt_path):
            with open(txt_path) as f:
                return f.read().strip()

        lines = [
            ln.strip() for ln in r.stdout.splitlines()
            if ln.strip() and not ln.strip().startswith("[")
        ]
        return " ".join(lines)

    except FileNotFoundError:
        log.error("whisper binary not found at %r", WHISPER_BIN)
        return ""
    except subprocess.TimeoutExpired:
        log.error("whisper timed out")
        return ""
    finally:
        try:
            os.unlink(txt_path)
        except OSError:
            pass


# ──────────────────────────────────────────────────────────────────────────────
# Complexity classifier
# ──────────────────────────────────────────────────────────────────────────────

_MATH_EXPR  = re.compile(r"\d+\s*[×÷+\-*/^]\s*\d+", re.UNICODE)
_MATH_QUERY = re.compile(r"what\s+is\s+\d|how\s+much\s+is\s+\d|what'?s\s+\d+", re.I)
_TIME_WORDS = re.compile(r"\b(time|date|today|tomorrow|yesterday|day|month|year|hour|minute)\b", re.I)
_UNIT_CONV  = re.compile(r"how\s+many\s+\w+\s+in\s+(?:a|an)\s+\w+|convert\s+\d|\d+\s+\w+\s+to\s+\w+", re.I)
_RESET_RE   = re.compile(
    r"\b(forget|reset|start.{0,4}over|fresh.{0,4}start|clear.{0,8}memory"
    r"|new.{0,8}conversation|forget.{0,8}everything|start.{0,4}fresh"
    r"|wipe.{0,8}memory|new.{0,4}chat)\b",
    re.I,
)

_SIMPLE_PHRASES: frozenset[str] = frozenset({
    "hello", "hi", "hey", "good morning", "good afternoon", "good evening",
    "how are you", "are you awake", "are you there",
    "what is your name", "what's your name", "who are you", "what can you do",
    "stop", "cancel", "nevermind", "thanks", "thank you",
    "okay", "ok", "yes", "no", "got it", "sounds good",
})


def classify(transcript: str) -> str:
    """Return 'local' for simple/fast queries, 'cloud' for complex ones."""
    t          = transcript.strip().lower().rstrip(".!? ")
    word_count = len(t.split())

    if t in _SIMPLE_PHRASES:
        return "local"
    if (_MATH_EXPR.search(transcript) or _MATH_QUERY.search(transcript)) and word_count <= 12:
        return "local"
    if _TIME_WORDS.search(transcript) and word_count <= 8:
        return "local"
    if _UNIT_CONV.search(transcript) and word_count <= 15:
        return "local"
    if word_count <= 7 and transcript.strip().endswith("?") and "." not in transcript[:-1]:
        return "local"

    return "cloud"


def needs_reset(transcript: str) -> bool:
    """Return True if the user wants to clear conversation memory."""
    return bool(_RESET_RE.search(transcript))


# ──────────────────────────────────────────────────────────────────────────────
# Conversation memory
# ──────────────────────────────────────────────────────────────────────────────

class ConversationExpired(Exception):
    """Raised when the stored conversation_id returns 404 from Arbiter."""


class ConversationManager:
    """Persists a single Arbiter conversation_id to disk.

    Cloud-tier turns are sent through /v1/conversations/:id/messages so the
    agent accumulates history across restarts.  Local-tier turns are always
    stateless and bypass this class entirely.

    Thread-safe: all mutable state is guarded by _lock.
    """

    def __init__(self, path: str, cloud_agent: str) -> None:
        self._path  = path
        self._agent = cloud_agent
        self._lock  = threading.Lock()
        self._id: int | None = self._load()
        if self._id is not None:
            log.info("conversation loaded id=%d from %s", self._id, path)
        else:
            log.info("no stored conversation — will create on first cloud turn")

    # ── public ────────────────────────────────────────────────────────────────

    def get_or_create(self) -> int:
        """Return the current conversation_id, creating one if needed."""
        with self._lock:
            if self._id is None:
                self._id = self._create()
                self._save()
            return self._id

    def reset(self) -> int:
        """Discard the current conversation and start a new one."""
        with self._lock:
            old = self._id
            self._id = self._create()
            self._save()
            log.info("conversation reset old=%s new=%d", old, self._id)
            return self._id

    def mark_expired(self, stale_id: int) -> None:
        """Called after a 404 — clears the ID so next call creates fresh."""
        with self._lock:
            if self._id == stale_id:
                self._id = None

    # ── private ───────────────────────────────────────────────────────────────

    def _load(self) -> int | None:
        try:
            with open(self._path) as f:
                return int(json.load(f)["conversation_id"])
        except (FileNotFoundError, KeyError, ValueError, json.JSONDecodeError):
            return None

    def _save(self) -> None:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(self._path)), exist_ok=True)
            with open(self._path, "w") as f:
                json.dump({"conversation_id": self._id}, f)
        except OSError as exc:
            log.warning("failed to save conversation state: %s", exc)

    def _create(self) -> int:
        body = json.dumps({"agent_id": self._agent}).encode()
        headers = {
            "Content-Type":   "application/json",
            "Content-Length": str(len(body)),
            "Authorization":  f"Bearer {ARBITER_TOKEN}",
        }
        conn = http.client.HTTPConnection(ARBITER_HOST, ARBITER_PORT, timeout=30)
        try:
            conn.request("POST", "/v1/conversations", body=body, headers=headers)
            resp = conn.getresponse()
            raw  = resp.read()
            if resp.status != 201:
                raise RuntimeError(
                    f"create conversation HTTP {resp.status}: {raw[:120]!r}"
                )
            return int(json.loads(raw)["id"])
        finally:
            conn.close()


# Module-level singleton.  Initialised in main() when BRIDGE_CONV_FILE is set.
_conv_mgr: ConversationManager | None = None


# ──────────────────────────────────────────────────────────────────────────────
# Arbiter SSE client
# ──────────────────────────────────────────────────────────────────────────────

def _parse_sse(resp: http.client.HTTPResponse) -> Iterator[str]:
    """Yield text deltas from an open Arbiter SSE response."""
    event_type = "message"
    data_lines: list[str] = []

    while True:
        raw = resp.readline()
        if not raw:
            break
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")

        if line.startswith(":"):
            continue

        if not line:
            if data_lines:
                try:
                    data = json.loads("\n".join(data_lines))
                except json.JSONDecodeError:
                    data = {}

                if event_type == "text":
                    delta = data.get("delta", "")
                    if delta:
                        yield delta
                elif event_type == "done":
                    return
                elif event_type == "error":
                    raise RuntimeError(data.get("error", "arbiter error"))

            event_type = "message"
            data_lines = []
            continue

        if line.startswith("event:"):
            event_type = line[6:].strip()
        elif line.startswith("data:"):
            data_lines.append(line[5:].strip())


def stream_arbiter_text(agent: str, message: str, idkey: str) -> Iterator[str]:
    """Stateless /v1/orchestrate — used for local-tier queries."""
    body = json.dumps({"agent": agent, "message": message}).encode()
    headers = {
        "Content-Type":    "application/json",
        "Content-Length":  str(len(body)),
        "Authorization":   f"Bearer {ARBITER_TOKEN}",
        "Idempotency-Key": idkey,
        "Accept":          "text/event-stream",
    }
    conn = http.client.HTTPConnection(ARBITER_HOST, ARBITER_PORT, timeout=120)
    try:
        conn.request("POST", "/v1/orchestrate", body=body, headers=headers)
        resp = conn.getresponse()
        if resp.status != 200:
            snippet = resp.read(256).decode("utf-8", errors="replace")
            raise RuntimeError(f"arbiter HTTP {resp.status}: {snippet}")
        yield from _parse_sse(resp)
    finally:
        conn.close()


def stream_conversation_text(
    conversation_id: int, message: str, idkey: str
) -> Iterator[str]:
    """Memory-enabled /v1/conversations/:id/messages — used for cloud-tier queries."""
    body = json.dumps({"message": message}).encode()
    headers = {
        "Content-Type":    "application/json",
        "Content-Length":  str(len(body)),
        "Authorization":   f"Bearer {ARBITER_TOKEN}",
        "Idempotency-Key": idkey,
        "Accept":          "text/event-stream",
    }
    path = f"/v1/conversations/{conversation_id}/messages"
    conn = http.client.HTTPConnection(ARBITER_HOST, ARBITER_PORT, timeout=120)
    try:
        conn.request("POST", path, body=body, headers=headers)
        resp = conn.getresponse()
        if resp.status == 404:
            resp.read()
            raise ConversationExpired()
        if resp.status != 200:
            snippet = resp.read(256).decode("utf-8", errors="replace")
            raise RuntimeError(f"arbiter HTTP {resp.status}: {snippet}")
        yield from _parse_sse(resp)
    finally:
        conn.close()


def _cloud_stream(message: str, idkey: str) -> Iterator[str]:
    """Route cloud-tier turn through conversation if memory is enabled, else stateless."""
    if _conv_mgr is None:
        yield from stream_arbiter_text(ARBITER_CLOUD, message, idkey)
        return

    for attempt in range(2):
        cid = _conv_mgr.get_or_create()
        try:
            yield from stream_conversation_text(cid, message, idkey)
            return
        except ConversationExpired:
            log.info("conversation %d expired (attempt %d), resetting", cid, attempt + 1)
            _conv_mgr.mark_expired(cid)

    raise RuntimeError("conversation unavailable after two attempts")


# ──────────────────────────────────────────────────────────────────────────────
# Sentence splitting
# ──────────────────────────────────────────────────────────────────────────────

_SENT_END = re.compile(r"(?<=[.!?])\s+")


def extract_sentences(buf: str) -> tuple[list[str], str]:
    """Return (complete sentences, trailing remainder) for progressive TTS."""
    parts = _SENT_END.split(buf)
    if len(parts) <= 1:
        return [], buf
    return parts[:-1], parts[-1]


# ──────────────────────────────────────────────────────────────────────────────
# Full turn pipeline
# ──────────────────────────────────────────────────────────────────────────────

def process_utterance(
    wav_bytes: bytes,
    *,
    source: str = "-",
    complexity_hint: str = "",
) -> tuple[bytes, str]:
    """Run the full pipeline; return (wav_bytes, transcript)."""
    turn_id = str(uuid.uuid4())
    t0      = time.monotonic()

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        tf.write(wav_bytes)
        wav_path = tf.name

    try:
        transcript = transcribe(wav_path)
    finally:
        try:
            os.unlink(wav_path)
        except OSError:
            pass

    t_stt = time.monotonic()
    log.info("turn=%s src=%s stt=%.0fms transcript=%r",
             turn_id, source, (t_stt - t0) * 1000, transcript[:80])

    if not transcript:
        return make_error_wav("I didn't catch that. Please try again."), ""

    # Classify — reset commands bypass the normal tier logic.
    if needs_reset(transcript):
        if _conv_mgr is not None:
            new_id = _conv_mgr.reset()
            log.info("turn=%s memory reset new_conversation=%d", turn_id, new_id)
        tier = "cloud"
        log.info("turn=%s tier=reset->cloud", turn_id)
        text_stream = _cloud_stream(transcript, turn_id)
    else:
        tier  = complexity_hint if complexity_hint in ("local", "cloud") else classify(transcript)
        log.info("turn=%s tier=%s", turn_id, tier)
        if tier == "local":
            text_stream = stream_arbiter_text(ARBITER_LOCAL, transcript, turn_id)
        else:
            text_stream = _cloud_stream(transcript, turn_id)

    # Stream Arbiter text; synthesise each sentence as it completes so Piper
    # runs concurrently with the model generating the next sentence.
    pcm_futures: list[Future[bytes]] = []
    text_buf = ""

    try:
        with ThreadPoolExecutor(max_workers=2) as tts_pool:
            for delta in text_stream:
                text_buf += delta
                sentences, text_buf = extract_sentences(text_buf)
                for sent in sentences:
                    pcm_futures.append(tts_pool.submit(tts_sentence, sent))

            if text_buf.strip():
                pcm_futures.append(tts_pool.submit(tts_sentence, text_buf.strip()))

            pcm_parts = [f.result() for f in pcm_futures]

    except RuntimeError as exc:
        log.error("turn=%s arbiter error: %s", turn_id, exc)
        return make_error_wav("I ran into a problem. Please try again."), transcript

    total_pcm = b"".join(p for p in pcm_parts if p)
    t_done    = time.monotonic()
    log.info("turn=%s total=%.0fms sentences=%d pcm_bytes=%d",
             turn_id, (t_done - t0) * 1000, len(pcm_futures), len(total_pcm))

    if not total_pcm:
        return make_error_wav("I don't have a response for that."), transcript

    return make_wav(total_pcm), transcript


# ──────────────────────────────────────────────────────────────────────────────
# HTTP handler
# ──────────────────────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    server_version = "arbiter-voice-bridge/1.0"

    def do_POST(self) -> None:
        if self.path == "/v1/utterance":
            self._handle_utterance()
        elif self.path == "/v1/transcribe":
            self._handle_transcribe()
        else:
            self.send_error(404, "not found")

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send_text(200, "ok\n")
        else:
            self.send_error(404, "not found")

    def _handle_utterance(self) -> None:
        body = self._read_audio_body()
        if body is None:
            return
        hint = self.headers.get("X-Complexity-Hint", "").lower().strip()
        wav, _ = process_utterance(body, source=self.client_address[0], complexity_hint=hint)
        self._send_wav(wav)

    def _handle_transcribe(self) -> None:
        """STT-only — useful for debug and latency measurement."""
        body = self._read_audio_body()
        if body is None:
            return
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
            tf.write(body)
            wav_path = tf.name
        try:
            transcript = transcribe(wav_path)
        finally:
            try:
                os.unlink(wav_path)
            except OSError:
                pass
        self._send_json(200, json.dumps({"transcript": transcript}).encode())

    def _read_audio_body(self) -> bytes | None:
        if not self._authorized():
            self.send_error(401, "unauthorized")
            return None
        ip = self.client_address[0]
        if not self._within_rate_limit(ip):
            self.send_error(429, "rate limit exceeded")
            return None
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            self.send_error(400, "invalid content-length")
            return None
        if length <= 0:
            self.send_error(400, "empty body")
            return None
        if length > BRIDGE_MAX_BYTES:
            self.send_error(413, "upload too large")
            return None
        body = self.rfile.read(length)
        if len(body) != length:
            self.send_error(400, "short read")
            return None
        return body

    def _authorized(self) -> bool:
        if not BRIDGE_API_KEY:
            return True
        return self.headers.get("Authorization", "") == f"Bearer {BRIDGE_API_KEY}"

    def _within_rate_limit(self, key: str) -> bool:
        now = time.monotonic()
        with _rate_lock:
            recent = [t for t in _rate_table.get(key, []) if now - t < _RATE_WINDOW]
            if len(recent) >= BRIDGE_RATE_LIMIT:
                _rate_table[key] = recent
                return False
            recent.append(now)
            _rate_table[key] = recent
            return True

    def _send_wav(self, wav: bytes) -> None:
        self.send_response(200)
        self.send_header("Content-Type",   "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.send_header("Connection",     "close")
        self.end_headers()
        self.wfile.write(wav)

    def _send_json(self, status: int, payload: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type",   "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection",     "close")
        self.end_headers()
        self.wfile.write(payload)

    def _send_text(self, status: int, text: str) -> None:
        body = text.encode()
        self.send_response(status)
        self.send_header("Content-Type",   "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        log.info("%s - %s", self.address_string(), fmt % args)


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

def main() -> int:
    global _conv_mgr

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-5s %(name)s %(message)s",
        datefmt="%H:%M:%S",
    )

    parser = argparse.ArgumentParser(description="Arbiter voice bridge")
    parser.add_argument("--host", default="")
    parser.add_argument("--port", type=int, default=8081)
    a = parser.parse_args()

    if not BRIDGE_API_KEY:
        log.warning("BRIDGE_API_KEY is not set — binding to loopback only")
        host = a.host or "127.0.0.1"
    else:
        host = a.host or "0.0.0.0"

    if BRIDGE_CONV_FILE:
        _conv_mgr = ConversationManager(BRIDGE_CONV_FILE, cloud_agent=ARBITER_CLOUD)
        log.info("memory=on  file=%s", BRIDGE_CONV_FILE)
    else:
        log.info("memory=off (set BRIDGE_CONVERSATION_FILE to enable)")

    log.info("arbiter=%s:%d  local=%s  cloud=%s",
             ARBITER_HOST, ARBITER_PORT, ARBITER_LOCAL, ARBITER_CLOUD)
    log.info("whisper bin=%-20s model=%s", WHISPER_BIN, WHISPER_MODEL)
    log.info("piper   bin=%-20s model=%s  sample_rate=%d",
             PIPER_BIN, PIPER_MODEL, PIPER_SAMPLE_RATE)

    server = ThreadingHTTPServer((host, a.port), Handler)
    log.info("listening on http://%s:%d", host, a.port)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
