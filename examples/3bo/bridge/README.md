# 3bo bridge

Two bridge scripts live here:

| File | Purpose |
| --- | --- |
| `bridge_stub.py` | Minimal bring-up stub — proves the Nano HTTP contract without running STT, Arbiter, or TTS. Returns a canned WAV. |
| `bridge.py` | Thin launcher — maps `THREEBO_*` env vars to the generic voice bridge and execs it. |

The full pipeline (STT → classify → Arbiter → TTS) lives in
[`examples/voice-bridge/bridge.py`](../../voice-bridge/bridge.py).

---

## Bring-up order

### Step 1 — hardware contract (bridge_stub.py)

Use the stub to verify the Nano↔Jetson HTTP contract before adding real
inference to the stack.

```sh
THREEBO_DEVICE_SECRET='replace-with-random-secret' \
python3 bridge_stub.py --host 0.0.0.0 --port 8081
```

Point the firmware `THREEBO_BRIDGE_BASE_URL` at `http://<jetson>:8081`.
Confirm the Nano uploads audio and plays back the canned response.

### Step 2 — full pipeline (bridge.py)

Once the hardware loop works, switch to the real bridge.

```sh
THREEBO_DEVICE_SECRET='replace-with-random-secret' \
THREEBO_ARBITER_TOKEN='atr_...' \
THREEBO_WHISPER_MODEL='/opt/3bo/models/ggml-base.en.bin' \
THREEBO_PIPER_MODEL='/opt/3bo/models/en_US-amy-low.onnx' \
python3 bridge.py --host 0.0.0.0 --port 8081
```

The real bridge uses `/v1/utterance` and `Authorization: Bearer <secret>`, not the stub's `/device/:id/utterance` and `X-3bo-Device-Secret`. The sketch already uses the correct endpoint and header. Rebuild and reflash the firmware before switching from the stub.

After reflashing, update `THREEBO_BRIDGE_BASE_URL` in `threebo_config.h` to point at the Jetson (e.g. `http://3bo.local:8081` requires avahi-daemon on the Jetson, or use the LAN IP directly).

---

## Environment variables

| 3bo variable | Maps to | Required |
| --- | --- | --- |
| `THREEBO_DEVICE_SECRET` | `BRIDGE_API_KEY` | yes |
| `THREEBO_ARBITER_TOKEN` | `ARBITER_TOKEN` | yes |
| `THREEBO_WHISPER_MODEL` | `WHISPER_MODEL` | yes |
| `THREEBO_PIPER_MODEL` | `PIPER_MODEL` | yes |
| `THREEBO_ARBITER_URL` | `ARBITER_URL` | default `http://127.0.0.1:8080` |
| `THREEBO_WHISPER_BIN` | `WHISPER_BIN` | default `whisper-cli` |
| `THREEBO_PIPER_BIN` | `PIPER_BIN` | default `piper` |
| `THREEBO_PIPER_SAMPLE_RATE` | `PIPER_SAMPLE_RATE` | default `16000` |
| `THREEBO_LOCAL_AGENT` | `ARBITER_LOCAL_AGENT` | default `local` |
| `THREEBO_CLOUD_AGENT` | `ARBITER_CLOUD_AGENT` | default `index` |

See [`examples/voice-bridge/bridge.py`](../../voice-bridge/bridge.py) for the
full list of generic options.
