# 3bo Jetson Orin brain

The Jetson Orin is 3bo's onboard Linux brain. It runs the software that is too
large or too credential-sensitive for the Nano ESP32: Arbiter, speech-to-text,
text-to-speech, the voice bridge, conversation state, logs, and model storage.

This document assumes the Jetson Orin Nano Super Developer Kit (JetPack 6,
Ubuntu 22.04, ARM64) unless a different module or carrier is noted.

---

## Role split

| Layer | Device | Responsibilities |
| --- | --- | --- |
| Body controller | Arduino Nano ESP32 | Wake trigger, I2S mic capture, LEDs, speaker playback, mute switch. Powered over Jetson USB. |
| Local brain | Jetson Orin | Arbiter daemon, Ollama, whisper.cpp STT, Piper TTS, 3bo voice bridge, logs, model storage. |

The Nano is deterministic and replaceable. The Jetson iterates like a normal
Linux service stack.

---

## Service map

| Service | Bind address | Port |
| --- | --- | --- |
| Arbiter API | `127.0.0.1` | `8080` |
| Ollama | `127.0.0.1` | `11434` |
| 3bo bridge | `0.0.0.0` | `8081` |
| whisper.cpp | subprocess (no port) | — |
| Piper | subprocess (no port) | — |

Arbiter and Ollama are loopback-only. The bridge is the only service that
accepts connections from the Nano or the LAN.

---

## Model directory

Put all model files under `/opt/3bo/models` so every service config points to
one location:

```sh
sudo mkdir -p /opt/3bo/models
sudo chown $USER /opt/3bo/models
```

Recommended layout after all installs:

```
/opt/3bo/models/
  ggml-tiny.en.bin        whisper.cpp — fast latency test
  ggml-base.en.bin        whisper.cpp — recommended first target
  en_US-amy-low.onnx      Piper voice model  (16 kHz output)
  en_US-amy-low.onnx.json Piper voice config (required alongside .onnx)
```

---

## 1. Jetson prerequisites

Flash JetPack 6 from SDK Manager or a prebuilt SD image. Confirm the device
boots, cooling is active, and SSH works before continuing.

Install build tools and Arbiter's library dependencies in one pass:

```sh
sudo apt update
sudo apt install -y \
  build-essential cmake git \
  libssl-dev libcurl4-openssl-dev libsqlite3-dev libreadline-dev \
  python3 python3-pip python3-venv
```

### mDNS hostname

The firmware uses `3bo.local` to reach the bridge. Set the Jetson's hostname
and enable the mDNS responder:

```sh
sudo hostnamectl set-hostname 3bo
sudo apt install -y avahi-daemon
sudo systemctl enable avahi-daemon
sudo systemctl start avahi-daemon
```

Confirm from another machine on the same LAN: `ping 3bo.local`

If you prefer a static IP instead of mDNS, set `THREEBO_BRIDGE_BASE_URL` to the
Jetson's IP address in firmware and skip avahi.

Confirm CUDA is available (Jetson ships with it in JetPack 6):

```sh
nvcc --version
```

---

## 2. Arbiter

### Build

```sh
git clone https://github.com/your-org/arbiter ~/arbiter
cd ~/arbiter
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo install -m 755 build/arbiter /usr/local/bin/arbiter
```

### Provider API keys

Arbiter reads provider keys from environment variables or files in
`~/.arbiter/`. Set the keys for any providers the robot will use:

```sh
# Anthropic (cloud agent — required for the index/cloud path)
echo "sk-ant-..." > ~/.arbiter/api_key
chmod 600 ~/.arbiter/api_key

# OpenAI (optional alternative cloud provider)
# echo "sk-..." > ~/.arbiter/openai_api_key

# Ollama is keyless — no file needed
```

Or export them as environment variables before starting the server:

```sh
export ANTHROPIC_API_KEY="sk-ant-..."
```

### First start

Run the server once interactively so you can see the admin token:

```sh
arbiter --api --bind 127.0.0.1 --port 8080
```

On first run Arbiter prints the admin token **once** — save it immediately:

```
Admin token (save this — not shown again):
  aat_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
Stored at: /home/jetson/.arbiter/admin_token (0600)
```

The admin token controls tenant provisioning. It is stored at
`~/.arbiter/admin_token` after the first run and reused on every subsequent
start.

### Provision a tenant token

Stop the server (`Ctrl-C`), then provision the 3bo tenant:

```sh
arbiter --add-tenant 3bo
```

Output:

```
Created tenant #1 (3bo)

  API key (save this — it will not be shown again):
    atr_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

Save the `atr_...` token. This is `THREEBO_ARBITER_TOKEN` for the bridge. The
database stores only a SHA-256 digest; a lost token means running
`arbiter --add-tenant 3bo` again to issue a new one.

### Restart

```sh
arbiter --api --bind 127.0.0.1 --port 8080
```

Verify with:

```sh
curl -s http://127.0.0.1:8080/v1/health
# {"status":"ok"}
```

---

## 3. Ollama (local fast-path model)

Ollama provides the local inference backend for simple queries, bypassing the
cloud entirely for arithmetic, time, greetings, and short factual questions.

### Install

```sh
curl -fsSL https://ollama.com/install.sh | sh
```

The installer creates a `ollama` systemd service that starts automatically.
Ollama listens on `localhost:11434` by default — no configuration needed for
the local case.

### Pull the fast-path model

```sh
ollama pull gemma3:4b
```

`gemma3:4b` fits in the Jetson Orin Nano's 8 GB and gives first-token latency
around 200–350 ms on quantized weights. For lower latency at some accuracy
cost, `gemma3:2b` is an option.

Confirm inference works:

```sh
ollama run gemma3:4b "What is 12 times 8?"
# 96
```

### Arbiter + Ollama

Arbiter routes `ollama/<model>` requests to `$OLLAMA_HOST` (default
`http://localhost:11434`). No additional configuration is needed when Ollama
and Arbiter run on the same machine.

---

## 4. whisper.cpp (STT)

### Build with CUDA

The Jetson's CUDA cores accelerate whisper inference significantly. Build
with `WHISPER_CUDA=ON`:

```sh
git clone https://github.com/ggml-org/whisper.cpp ~/whisper.cpp
cd ~/whisper.cpp
cmake -B build -DWHISPER_CUDA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo install -m 755 build/bin/whisper-cli /usr/local/bin/whisper-cli
```

If the CUDA build fails, drop the flag for a CPU-only build:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

CPU inference is slower (~600–900 ms for `base.en` on a 4 s clip) but
correct.

### Download models

whisper.cpp ships a download script for the standard ggml models:

```sh
cd ~/whisper.cpp
bash models/download-ggml-model.sh tiny.en
bash models/download-ggml-model.sh base.en
cp models/ggml-tiny.en.bin models/ggml-base.en.bin /opt/3bo/models/
```

### Latency reference

These times are approximate on the Jetson Orin Nano with a 4 s utterance and
the CUDA build:

| Model | VRAM | Latency | Notes |
| --- | --- | --- | --- |
| `tiny.en` | ~75 MB | ~80–120 ms | Fastest; lower accuracy on noisy input |
| `base.en` | ~145 MB | ~150–250 ms | Recommended starting point |
| `small.en` | ~465 MB | ~400–600 ms | Better accuracy; measure thermals first |

Start with `base.en`. Move to `tiny.en` if latency tests show the STT step is
dominating; move to `small.en` if recognition accuracy is the bottleneck.

### Smoke test

```sh
# Record a short clip on any machine and copy it to the Jetson, or use
# a saved WAV. The file must be 16 kHz mono signed-16-bit PCM.
whisper-cli \
  -m /opt/3bo/models/ggml-base.en.bin \
  -f /path/to/test.wav \
  -nt -l en
```

Expected output is the transcript on stdout with no timestamps.

---

## 5. Piper (TTS)

Piper is a lightweight neural TTS engine that runs well on ARM without a GPU.
Use a `low` quality voice for 16 kHz output that matches the ESP32 playback
pipeline.

### Install

```sh
pip3 install piper-tts
```

If `pip` resolves an incompatible wheel for your Python version, download the
ARM64 release binary directly from the Piper GitHub releases page and install
it to `/usr/local/bin/piper`.

### Download the voice model

Each Piper voice consists of two files: an `.onnx` model and a `.onnx.json`
config. Both must be in the same directory.

Visit the [Piper voices repository](https://github.com/rhasspy/piper) and
download a `low` quality English voice. `en_US-amy-low` outputs 16 kHz, which
matches the ESP32 firmware's expected sample rate:

```sh
# Download amy-low (16 kHz output — matches firmware)
cd /opt/3bo/models

# .onnx model
wget -q "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/low/en_US-amy-low.onnx"

# .onnx.json config (required alongside the model)
wget -q "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/low/en_US-amy-low.onnx.json"
```

`low` quality voices output 16 kHz PCM. `medium` and `high` voices output
22 050 Hz. If you use a higher-quality voice, set `THREEBO_PIPER_SAMPLE_RATE`
to match and confirm the WAV header in the bridge response matches what the
ESP32 expects.

### Smoke test

Piper with `--output_raw` writes raw signed-16-bit PCM to stdout. Pipe it
through `aplay` or `sox` to verify the voice sounds correct before wiring it
into the bridge:

```sh
echo "Hello, I am 3bo." | piper \
  --model /opt/3bo/models/en_US-amy-low.onnx \
  --output_raw | aplay -r 16000 -f S16_LE -c 1
```

---

## 6. Bridge

### Configure environment variables

Create an env file at `/etc/3bo/bridge.env` (or any secure path):

```sh
sudo mkdir -p /etc/3bo
sudo tee /etc/3bo/bridge.env > /dev/null <<'EOF'
# Auth — must match THREEBO_DEVICE_SECRET in threebo_config.h
THREEBO_DEVICE_SECRET=replace-with-a-random-secret

# Arbiter — the atr_... token from `arbiter --add-tenant 3bo`
THREEBO_ARBITER_TOKEN=atr_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

# STT
THREEBO_WHISPER_MODEL=/opt/3bo/models/ggml-base.en.bin

# TTS
THREEBO_PIPER_MODEL=/opt/3bo/models/en_US-amy-low.onnx
# THREEBO_PIPER_SAMPLE_RATE=16000   (default; change if using a medium/high voice)

# Conversation memory — saves conversation_id across bridge restarts
THREEBO_CONVERSATION_FILE=/etc/3bo/conversation.json

# Agent routing (defaults match what we create below)
# THREEBO_LOCAL_AGENT=local
# THREEBO_CLOUD_AGENT=index

# Hardware event forwarding — bridge relays firmware state events to Arbiter.
# Set to 0 to disable if you don't register the threebo-monitor agent.
# BRIDGE_EVENTS_ENABLED=1
EOF
sudo chmod 600 /etc/3bo/bridge.env
```

### Register the Arbiter agents

Arbiter must be running for these commands.

**Local fast-path agent** — sends simple queries to Ollama instead of the cloud:

```sh
curl -s -X POST http://127.0.0.1:8080/v1/agents \
  -H "Authorization: Bearer atr_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
  -H "Content-Type: application/json" \
  -d '{
    "id":          "local",
    "role":        "quick-responder",
    "model":       "ollama/gemma3:4b",
    "max_tokens":  256,
    "temperature": 0.2,
    "goal":        "Answer simple, short questions in one or two sentences. Be direct and concise. Do not add preamble."
  }'
```

The `index` agent (cloud) already exists as the default orchestrator. No
additional registration is needed unless you want to override its model or
goal.

**Hardware event monitor** — receives device state events from the firmware
via `POST /v1/events` and handles them with a local Ollama model.  Matched by
the `event_types` glob patterns in the agent JSON
(`examples/3bo/agents/threebo-monitor.json`):

```sh
curl -s -X POST http://127.0.0.1:8080/v1/agents \
  -H "Authorization: Bearer atr_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
  -H "Content-Type: application/json" \
  -d @/path/to/arbiter/examples/3bo/agents/threebo-monitor.json
```

The monitor agent handles `device.*` and `audio.*` event types locally (no
cloud call) and logs errors to the agent's `/mem` store.  The bridge must have
`BRIDGE_EVENTS_ENABLED=1` (the default) for event forwarding to be active.

### Run

```sh
cd /path/to/arbiter/examples/3bo/bridge
source /etc/3bo/bridge.env
python3 bridge.py --host 0.0.0.0 --port 8081
```

### End-to-end smoke test

Test each stage independently before connecting the Nano.

**STT only:**

```sh
source /etc/3bo/bridge.env
curl -s -X POST http://localhost:8081/v1/transcribe \
  -H "Authorization: Bearer $THREEBO_DEVICE_SECRET" \
  -H "Content-Type: audio/wav" \
  --data-binary @/path/to/test.wav
# {"transcript":"what time is it"}
```

**Full utterance pipeline:**

```sh
curl -s -X POST http://localhost:8081/v1/utterance \
  -H "Authorization: Bearer $THREEBO_DEVICE_SECRET" \
  -H "Content-Type: audio/wav" \
  --data-binary @/path/to/test.wav \
  --output response.wav
aplay -r 16000 -f S16_LE -c 1 response.wav
```

**Unauthenticated rejection check:**

```sh
curl -s -o /dev/null -w "%{http_code}" \
  -X POST http://localhost:8081/v1/utterance \
  -H "Content-Type: audio/wav" \
  --data-binary @/path/to/test.wav
# 401
```

**Hardware event endpoint:**

```sh
curl -s -X POST http://localhost:8081/v1/event \
  -H "Authorization: Bearer $THREEBO_DEVICE_SECRET" \
  -H "Content-Type: application/json" \
  -d '{"type":"device.mute","data":{}}'
# {"ok":true}
```

---

## 7. Systemd services

### Arbiter

```sh
sudo tee /etc/systemd/system/arbiter-api.service > /dev/null <<'EOF'
[Unit]
Description=Arbiter API server
After=network.target
Wants=ollama.service

[Service]
Type=simple
User=jetson
EnvironmentFile=/etc/3bo/bridge.env
Environment=ANTHROPIC_API_KEY=sk-ant-...
ExecStart=/usr/local/bin/arbiter --api --bind 127.0.0.1 --port 8080
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
```

Put the Anthropic key in the `Environment=` line above, or add it to
`/etc/3bo/bridge.env` and reference it there. Do not put provider keys in
`~/.arbiter/api_key` when running as a system service under a different user.

### 3bo bridge

```sh
sudo tee /etc/systemd/system/3bo-bridge.service > /dev/null <<'EOF'
[Unit]
Description=3bo voice bridge
After=arbiter-api.service ollama.service
Requires=arbiter-api.service

[Service]
Type=simple
User=jetson
EnvironmentFile=/etc/3bo/bridge.env
WorkingDirectory=/home/jetson/arbiter/examples/3bo/bridge
ExecStart=/usr/bin/python3 bridge.py --host 0.0.0.0 --port 8081
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
```

### Enable and start

```sh
sudo systemctl daemon-reload
sudo systemctl enable ollama arbiter-api 3bo-bridge
sudo systemctl start ollama arbiter-api 3bo-bridge
sudo systemctl status arbiter-api 3bo-bridge
```

### Log tailing

```sh
journalctl -fu arbiter-api
journalctl -fu 3bo-bridge
```

---

## 8. Performance tuning

### Jetson power mode

The Jetson Orin Nano has several power profiles. Use `15W` or higher for
inference work:

```sh
sudo nvpmodel -m 0     # MAXN (full power)
sudo jetson_clocks     # lock CPU/GPU clocks at max
```

Check current mode:

```sh
sudo nvpmodel -q
```

For battery-powered deployment, `10W` is a reasonable compromise between
latency and power draw. Measure first-token latency under each mode before
committing to a target.

### Latency budget

Approximate end-to-end latency for a typical 4 s utterance on `MAXN` with
`ggml-base.en` and Gemma 3 4B for a local-tier query:

| Stage | Time |
| --- | --- |
| Wake detection (ESP32) | ~10 ms |
| Audio upload over USB/Wi-Fi | ~50–150 ms |
| STT (`base.en`, CUDA) | ~150–250 ms |
| Complexity classification | < 1 ms |
| Arbiter → Ollama TTFT (`gemma3:4b`) | ~200–350 ms |
| Piper first sentence | ~80–120 ms |
| Audio return to ESP32 | ~30–80 ms |
| **Total (local tier)** | **~520–960 ms** |

For cloud-tier queries replace the Ollama TTFT row with ~400–700 ms for
Claude, and all other rows remain the same.

The bridge pipelines Piper synthesis with model generation — sentences 1–N−1
synthesise while the model generates sentence N — so TTS is not fully
serialised after the model finishes.

### Whisper model selection

Switch models by changing `THREEBO_WHISPER_MODEL` and restarting the bridge:

```sh
# Faster, slightly less accurate
THREEBO_WHISPER_MODEL=/opt/3bo/models/ggml-tiny.en.bin

# More accurate, slower
THREEBO_WHISPER_MODEL=/opt/3bo/models/ggml-small.en.bin
```

Run `/v1/transcribe` tests with representative 3bo utterances to measure
both accuracy and latency before switching in production.

---

## Bring-up checklist

Run these steps in order. Each depends on the previous.

### Step 1 — platform

- [ ] Flash JetPack 6 and boot.
- [ ] Confirm active cooling (`jtop` or `tegrastats`).
- [ ] Enable SSH and confirm network access.
- [ ] Install system packages (`build-essential cmake libssl-dev` …).

### Step 2 — Arbiter

- [ ] Build Arbiter and install to `/usr/local/bin/arbiter`.
- [ ] Write provider key to `~/.arbiter/api_key`.
- [ ] Run `arbiter --api` once interactively; save the admin token.
- [ ] Run `arbiter --add-tenant 3bo`; save the `atr_...` token.
- [ ] Confirm `curl http://127.0.0.1:8080/v1/health` returns `{"status":"ok"}`.

### Step 3 — Ollama

- [ ] Install Ollama.
- [ ] Pull `gemma3:4b`.
- [ ] Confirm `ollama run gemma3:4b "What is 7 times 6?"` returns `42`.

### Step 4 — whisper.cpp

- [ ] Build with `WHISPER_CUDA=ON`.
- [ ] Download `ggml-base.en.bin` to `/opt/3bo/models/`.
- [ ] Run `whisper-cli -m /opt/3bo/models/ggml-base.en.bin -f test.wav -nt` and confirm transcript.

### Step 5 — Piper

- [ ] Install `piper-tts`.
- [ ] Download `en_US-amy-low.onnx` and `en_US-amy-low.onnx.json` to `/opt/3bo/models/`.
- [ ] Smoke test: `echo "Hello." | piper --model ... --output_raw | aplay -r 16000 -f S16_LE -c 1`.

### Step 6 — bridge

- [ ] Write `/etc/3bo/bridge.env` with all required variables.
- [ ] Register the `local` Arbiter agent via `curl POST /v1/agents`.
- [ ] Register the `threebo-monitor` event agent via `curl POST /v1/agents` with `examples/3bo/agents/threebo-monitor.json`.
- [ ] Start the bridge: `python3 bridge.py --host 0.0.0.0 --port 8081`.
- [ ] Confirm `GET /health` returns `ok`.
- [ ] Confirm unauthenticated `POST /v1/utterance` returns `401`.
- [ ] Run authenticated `/v1/transcribe` test and confirm transcript.
- [ ] Run authenticated `/v1/utterance` test and play back the WAV.
- [ ] Send a test hardware event: `curl -s -X POST .../v1/event -d '{"type":"device.mute","data":{}}'` and confirm `{"ok":true}`.

### Step 7 — hardware loop

- [ ] Flash firmware with `THREEBO_BRIDGE_BASE_URL` pointing at the Jetson.
- [ ] Send a serial wake (`w` over Serial) from the Arduino IDE monitor.
- [ ] Confirm the Nano records, uploads, receives a WAV, and plays it.
- [ ] Confirm LED states transition: Idle → Wake → Listening → Uploading → Thinking → Speaking → Idle.

### Step 8 — services

- [ ] Install `arbiter-api.service` and `3bo-bridge.service` unit files.
- [ ] Enable and start all services.
- [ ] Reboot the Jetson and confirm all services start automatically.
- [ ] Run the hardware loop test again after reboot.

---

## 9. Vision service (Milestone 5 — planned)

> Not needed for the v1 voice prototype. Complete Milestones 1–8 first.
> Full design specification: [VISION.md](VISION.md).
> Wiring: [CIRCUIT.md](CIRCUIT.md) — PCA9685 and camera sections.
> Parts: [BOM.md](BOM.md) — Vision and motion subsystem table.

Additional dependencies when Milestone 5 begins:

```sh
pip3 install mediapipe opencv-python smbus2 pyserial
ollama pull moondream
```

---

## Source references

- NVIDIA Jetson Orin Nano Super Developer Kit: https://www.nvidia.com/en-us/autonomous-machines/embedded-systems/jetson-orin/nano-super-developer-kit/
- whisper.cpp: https://github.com/ggml-org/whisper.cpp
- Piper TTS: https://github.com/rhasspy/piper
- Ollama: https://ollama.com
- MediaPipe: https://developers.google.com/mediapipe
- Piper voices (Hugging Face): https://huggingface.co/rhasspy/piper-voices
