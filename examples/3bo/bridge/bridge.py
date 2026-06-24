#!/usr/bin/env python3
"""3bo bridge launcher.

Maps 3bo-specific env vars to the generic voice bridge and runs it.
The full pipeline (STT → classify → memory → Arbiter → TTS) lives in
examples/voice-bridge/bridge.py.

Required:
  THREEBO_DEVICE_SECRET      used as BRIDGE_API_KEY (Authorization: Bearer)
  THREEBO_ARBITER_TOKEN      used as ARBITER_TOKEN
  THREEBO_WHISPER_MODEL      used as WHISPER_MODEL
  THREEBO_PIPER_MODEL        used as PIPER_MODEL

Optional:
  THREEBO_ARBITER_URL        maps to ARBITER_URL          (default http://127.0.0.1:8080)
  THREEBO_WHISPER_BIN        maps to WHISPER_BIN          (default whisper-cli)
  THREEBO_PIPER_BIN          maps to PIPER_BIN            (default piper)
  THREEBO_PIPER_SAMPLE_RATE  maps to PIPER_SAMPLE_RATE    (default 16000)
  THREEBO_LOCAL_AGENT        maps to ARBITER_LOCAL_AGENT  (default local)
  THREEBO_CLOUD_AGENT        maps to ARBITER_CLOUD_AGENT  (default index)
  THREEBO_CONVERSATION_FILE  maps to BRIDGE_CONVERSATION_FILE
                             Path to persist conversation_id across restarts.
                             Recommended: /etc/3bo/conversation.json
                             When set, cloud-tier turns have persistent memory.
                             Say "forget everything" or "start fresh" to reset.

The Nano ESP32 firmware sends POST /v1/utterance with:
  Authorization: Bearer <THREEBO_DEVICE_SECRET>
The firmware source uses THREEBO_BRIDGE_BASE_URL from threebo_config.h.
The stub used /device/:id/utterance — make sure threebo_config.h points at
the real bridge URL and that firmware has been rebuilt after that change.
"""

import os
import sys

_MAP = {
    "BRIDGE_API_KEY":           "THREEBO_DEVICE_SECRET",
    "ARBITER_TOKEN":            "THREEBO_ARBITER_TOKEN",
    "WHISPER_MODEL":            "THREEBO_WHISPER_MODEL",
    "PIPER_MODEL":              "THREEBO_PIPER_MODEL",
    "ARBITER_URL":              "THREEBO_ARBITER_URL",
    "WHISPER_BIN":              "THREEBO_WHISPER_BIN",
    "PIPER_BIN":                "THREEBO_PIPER_BIN",
    "PIPER_SAMPLE_RATE":        "THREEBO_PIPER_SAMPLE_RATE",
    "ARBITER_LOCAL_AGENT":      "THREEBO_LOCAL_AGENT",
    "ARBITER_CLOUD_AGENT":      "THREEBO_CLOUD_AGENT",
    "BRIDGE_CONVERSATION_FILE": "THREEBO_CONVERSATION_FILE",
}

for generic, threebo in _MAP.items():
    if threebo in os.environ and generic not in os.environ:
        os.environ[generic] = os.environ[threebo]

_here    = os.path.dirname(os.path.abspath(__file__))
_generic = os.path.join(_here, "..", "..", "voice-bridge", "bridge.py")

if not os.path.exists(_generic):
    sys.exit(f"generic bridge not found at {_generic}")

os.execv(sys.executable, [sys.executable, _generic] + sys.argv[1:])
