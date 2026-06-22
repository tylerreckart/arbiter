#!/usr/bin/env python3
"""Minimal 3bo Jetson bridge contract stub.

This accepts the Nano ESP32 bench firmware's WAV upload and returns a small
16 kHz mono WAV. It does not run STT, Arbiter, or TTS yet; it exists to verify
networking, auth, upload caps, response playback, and LED state transitions.
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO


MAX_UPLOAD_BYTES = 512 * 1024
RATE_WINDOW_SECONDS = 60
RATE_MAX_REQUESTS = 12

REQUEST_TIMES: dict[str, list[float]] = {}


def make_test_wav() -> bytes:
    sample_rate = 16_000
    duration_seconds = 0.45
    frequency_hz = 660
    frames = int(sample_rate * duration_seconds)

    pcm = bytearray()
    for i in range(frames):
        envelope = min(1.0, i / 800, (frames - i) / 1200)
        sample = int(4500 * envelope * math.sin(2 * math.pi * frequency_hz * i / sample_rate))
        pcm.extend(struct.pack("<h", sample))

    out = BytesIO()
    with wave.open(out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(bytes(pcm))
    return out.getvalue()


TEST_WAV = make_test_wav()


class Handler(BaseHTTPRequestHandler):
    server_version = "threebo-bridge-stub/0.1"

    def do_POST(self) -> None:
        parts = [p for p in self.path.split("/") if p]
        if len(parts) != 3 or parts[0] != "device" or parts[2] != "utterance":
            self.send_error(404, "not found")
            return

        device_id = parts[1]
        if not self._authorized():
            self.send_error(401, "missing or invalid device secret")
            return

        if not self._within_rate_limit(device_id):
            self.send_error(429, "device rate limit exceeded")
            return

        content_length = self.headers.get("Content-Length")
        try:
            length = int(content_length or "0")
        except ValueError:
            self.send_error(400, "invalid content length")
            return

        if length <= 0:
            self.send_error(400, "empty utterance")
            return
        if length > MAX_UPLOAD_BYTES:
            self.send_error(413, "utterance too large")
            return

        body = self.rfile.read(length)
        if len(body) != length:
            self.send_error(400, "short upload")
            return

        print(f"device={device_id} wav_bytes={len(body)} sample_rate={self.headers.get('X-Sample-Rate')}")

        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(TEST_WAV)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(TEST_WAV)

    def do_GET(self) -> None:
        if self.path == "/health":
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404, "not found")

    def _authorized(self) -> bool:
        expected = self.server.device_secret  # type: ignore[attr-defined]
        got = self.headers.get("X-3bo-Device-Secret", "")
        return bool(expected) and got == expected

    def _within_rate_limit(self, device_id: str) -> bool:
        now = time.monotonic()
        recent = [t for t in REQUEST_TIMES.get(device_id, []) if now - t < RATE_WINDOW_SECONDS]
        if len(recent) >= RATE_MAX_REQUESTS:
            REQUEST_TIMES[device_id] = recent
            return False
        recent.append(now)
        REQUEST_TIMES[device_id] = recent
        return True

    def log_message(self, fmt: str, *args: object) -> None:
        print("%s - %s" % (self.address_string(), fmt % args))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    args = parser.parse_args()

    secret = os.environ.get("THREEBO_DEVICE_SECRET", "")
    if not secret:
        raise SystemExit("set THREEBO_DEVICE_SECRET before starting the bridge stub")

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.device_secret = secret  # type: ignore[attr-defined]
    print(f"3bo bridge stub listening on http://{args.host}:{args.port}")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
