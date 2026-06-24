# 3bo Vision and Head-Tracking System

> **MILESTONE 5 — FUTURE DESIGN**
> Nothing in this document is implemented in v1. The v1 prototype has no camera,
> no servos, and no PCA9685. Build servo pockets into the v1 neck bracket so
> Milestone 5 hardware can be installed without a mechanical rebuild.

---

## Overview

3bo detects a human face on wake and orients toward the user throughout the
conversation. All horizontal tracking is handled by the motorised base. Vertical
tracking uses a three-arm differential neck: one passive rear ball-joint and two
servo-driven push/pull rods at the front. The head camera also enables visual
queries — the user can ask what 3bo sees and the response draws on a live frame
passed through a local VLM (moondream2 via Ollama).

---

## Additional Hardware

| Component | Part | Notes |
|---|---|---|
| Camera | Adafruit OV5640 Camera Breakout — 72° Lens with Autofocus, product 5945 | Mounted in robot head. ESP32-S3 JPEG-compresses frames and forwards to Jetson over USB serial. |
| PWM driver | Adafruit PCA9685 16-channel servo driver, product 815 | I2C 0x40; connected to Jetson 40-pin header |
| Servo L | MG90S metal-gear micro servo | PCA9685 ch 0. Left push/pull rod. |
| Servo R | MG90S metal-gear micro servo | PCA9685 ch 1. Right push/pull rod. |
| Base motor | N20 6 V gearmotor with quadrature encoder, 100–200 RPM | DRV8833 H-bridge; IN1/IN2 from PCA9685 ch 2/3 |
| Base bearing | Lazy Susan bearing, 100–150 mm | Supports full body weight through 360° rotation |
| Slip ring | 12-wire capsule slip ring, ≥ 2 A/circuit | Passes 19 V supply, 5 V body rail, motor control, and encoder signals through the rotating base joint |

See [CIRCUIT.md](CIRCUIT.md) for wiring details and [BOM.md](BOM.md) for the full vision subsystem parts list.

---

## Neck Mechanism

Three-arm differential: one passive rear pivot (ball joint at head CG height)
and two servo-driven push/pull rods at the front (35 mm apart).

Servo mixing:
- Both servos together → pitch (nod, ±30°)
- Differential servo motion → roll (head-cock, ±15°)
- All horizontal tracking → base yaw (N20 motor, 360° continuous)

Both servos mount at the neck base, not in the head, to keep rotational inertia
low. Rods run up through the neck tube to the head attachment points.

---

## Vision Service

A background process (`vision_service.py`, not yet written) will run on the
Jetson and expose a small localhost HTTP API.

| Endpoint | Description |
|---|---|
| `GET /face` | Current face centroid, confidence, and servo positions |
| `GET /frame` | Latest JPEG from the OV5640 |
| `POST /track` | Enable or disable PID servo tracking |
| `POST /home` | Drive head to level neutral position |
| `POST /rest` | Drive head to rest pose (−5° pitch, 0° roll) |
| `GET /health` | Liveness check |

Face detection uses MediaPipe FaceDetector. The PID loop converts centroid
error to pitch (servos) and yaw (base motor) commands. The head has no pan
axis — horizontal tracking is base yaw only.

---

## Visual Queries

When the user asks what 3bo sees ("what's in front of you", "describe this",
"what is that"):

1. Bridge calls `GET /frame` for the latest JPEG.
2. Frame is passed to `moondream` via Ollama (`ollama pull moondream`).
3. Prompt: *"Describe what you see concisely in two sentences."*
4. Description is prepended to the Arbiter message as `[Visual context: ...]`.
5. Query routes to the cloud agent regardless of the complexity classifier.

---

## On Wake

When the bridge receives `audio.wake_detected`:

1. Base sweeps from −45° to +45° yaw at ~20°/s (scan state).
2. If face detected (confidence > 0.85), PID tracking activates.
3. After lock or 2.5 s timeout, transition to listening.
4. On idle return: call `/rest`, disable tracking.

---

## Milestone Placement

| Milestone | Scope |
|---|---|
| 1–4 | Voice prototype (current) — no camera, servos, or PCA9685 |
| **5** | **This document — camera, head-tracking, VLM visual queries** |
