# Arbiter voice bridge

A generic bridge between any audio-capable device and a locally-running
Arbiter API, using whisper.cpp for speech-to-text and Piper for
text-to-speech.

```
device (WAV upload)
  → whisper.cpp  (STT)
  → classifier   (local vs cloud, ~0 ms, rule-based)
  → Arbiter SSE  (local Ollama agent or cloud agent)
  → Piper        (TTS, sentence-concurrent with generation)
  → device (WAV response)
```

## Requirements

| Tool | Install |
| --- | --- |
| whisper.cpp | Build from source: `cmake -B build && cmake --build build -j$(nproc)` |
| Piper | `pip install piper-tts` or download a release binary |
| Arbiter | `arbiter --api --bind 127.0.0.1 --port 8080` |
| Ollama (optional) | For the local fast-path agent |

## Quick start

```sh
# Pull a local model for simple queries (optional but recommended)
ollama pull gemma3:4b

# Start Arbiter
arbiter --api &

# Register the local agent
curl -sX POST http://127.0.0.1:8080/v1/agents \
  -H "Authorization: Bearer $ARBITER_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"id":"local","model":"ollama/gemma3:4b","max_tokens":256,
       "goal":"Answer simple questions concisely in one or two sentences."}'

# Start the bridge
ARBITER_TOKEN='atr_...' \
WHISPER_MODEL='/path/to/ggml-base.en.bin' \
PIPER_MODEL='/path/to/en_US-amy-low.onnx' \
BRIDGE_API_KEY='your-secret' \
python3 bridge.py --port 8081
```

## Endpoints

| Method | Path | Description |
| --- | --- | --- |
| `POST` | `/v1/utterance` | WAV in → WAV out (main device endpoint) |
| `POST` | `/v1/transcribe` | WAV in → `{"transcript":"..."}` (debug/latency test) |
| `GET` | `/health` | `200 ok` liveness check |

### POST /v1/utterance

Upload 16 kHz mono signed-16-bit WAV, receive a WAV response.

```http
POST /v1/utterance HTTP/1.1
Authorization: Bearer <BRIDGE_API_KEY>
Content-Type: audio/wav
Content-Length: <bytes>
X-Complexity-Hint: local   (optional — override the classifier)
```

Response: `audio/wav` with `Content-Length`.

### POST /v1/transcribe

Same upload format; returns JSON instead of audio.  Useful for measuring
STT latency before wiring the full pipeline.

```json
{"transcript": "what time is it"}
```

## Environment variables

| Variable | Required | Default | Description |
| --- | --- | --- | --- |
| `ARBITER_TOKEN` | yes | — | Arbiter bearer token (`atr_...`) |
| `WHISPER_MODEL` | yes | — | Path to whisper.cpp ggml model file |
| `PIPER_MODEL` | yes | — | Path to Piper `.onnx` voice model |
| `ARBITER_URL` | no | `http://127.0.0.1:8080` | Arbiter API base URL |
| `ARBITER_LOCAL_AGENT` | no | `local` | Agent ID for simple queries |
| `ARBITER_CLOUD_AGENT` | no | `index` | Agent ID for complex queries |
| `WHISPER_BIN` | no | `whisper-cli` | whisper.cpp binary name or path |
| `PIPER_BIN` | no | `piper` | Piper binary name or path |
| `PIPER_SAMPLE_RATE` | no | `16000` | Must match the Piper model's output rate |
| `BRIDGE_API_KEY` | no | — | If set, require `Authorization: Bearer <key>` |
| `BRIDGE_MAX_BYTES` | no | `524288` | Max upload size in bytes |
| `BRIDGE_RATE_LIMIT` | no | `20` | Max requests per source IP per 60 s |

If `BRIDGE_API_KEY` is unset, the bridge warns at startup and binds to
`127.0.0.1` only, regardless of `--host`.

## Complexity routing

The bridge classifies each transcript with a fast rule-based heuristic
(~0 ms, no model call) and routes to one of two Arbiter agents:

| Tier | Examples | Agent |
| --- | --- | --- |
| `local` | arithmetic, short time/date queries, greetings | `ARBITER_LOCAL_AGENT` |
| `cloud` | multi-sentence reasoning, planning, open-ended | `ARBITER_CLOUD_AGENT` |

Pass `X-Complexity-Hint: local` or `X-Complexity-Hint: cloud` to bypass
the classifier from the device side.

## Latency pipeline

The bridge pipelines Arbiter text generation with Piper synthesis: each
sentence is submitted to Piper as soon as it completes, while the model
is still generating subsequent sentences.  For a four-sentence response,
sentences 1–3 synthesise while the model finishes sentence 4, so the
bottleneck is `max(model_time, last_sentence_tts_time)` rather than
`model_time + total_tts_time`.

The response is returned only after all synthesis completes (firmware
needs `Content-Length` up front).

## Hardware examples

- **3bo robot** — see [`examples/3bo/bridge/`](../3bo/bridge/) for the
  thin `THREEBO_*` env-var shim that maps 3bo config to this bridge.
- Any ESP32, Raspberry Pi, or other device that can POST a WAV over HTTP
  and play a WAV response works without modification.
