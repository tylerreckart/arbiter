# 3bo Vision and Head-Tracking System

> **MILESTONE 5 — FUTURE DESIGN ONLY**
> Nothing in this document is implemented in v1. The v1 prototype has no camera,
> no servos, and no PCA9685. This document is a design specification for a
> future milestone. Do not treat any section as actionable guidance until
> Milestone 5 begins.

---

## Overview

This document describes the planned camera and head-tracking system for 3bo.

3bo detects a human face on wake and orients toward the user throughout the conversation. The neck uses a three-arm differential mechanism: a passive ball-joint pivot at the rear of the head and two servo-driven push/pull rods at the front. Both servos together control pitch (nod); differential servo motion controls roll (head cock). All horizontal tracking is handled by the motorised base. The head camera enables visual queries — the user can ask what 3bo sees and the response draws on a live frame passed through a local VLM.

The system adds a USB webcam on the head tier, a PCA9685 I2C PWM driver on the Jetson, two MG90S micro-servos driving push/pull rods from the neck base to the head, an N20 gearmotor with encoder for base rotation, and a slip ring at the base joint for continuous 360° rotation.
A background vision service runs on the Jetson and exposes a small localhost
HTTP API consumed by the bridge.

---

## Hardware (Planned)

### Component Table

| Component | Part | Notes |
|---|---|---|
| Camera | Adafruit OV5640 Camera Breakout — 72° Lens with Autofocus, product 5945 | Mounted in robot head on custom carrier board. 8-bit parallel DVP to ESP32-S3 camera peripheral. ESP32-S3 JPEG-compresses frames and forwards them to Jetson over USB serial. |
| PWM driver | PCA9685 16-channel I2C servo driver | I2C address 0x40; connected to Jetson 40-pin header I2C bus |
| Servo L | MG90S metal-gear micro servo | PCA9685 channel 0. Left push/pull rod. |
| Servo R | MG90S metal-gear micro servo | PCA9685 channel 1. Right push/pull rod. |
| Base motor | N20 gearmotor with quadrature encoder, 6 V, 100–200 RPM | DRV8833 H-bridge driver; IN1/IN2 from PCA9685 channels 2/3. Encoder A/B to Jetson GPIO. |
| Base bearing | Lazy Susan ball bearing, 100–150 mm | Supports full body weight through 360° rotation |
| Slip ring | 12-wire capsule slip ring, ≥ 2 A/circuit | Passes 19 V supply, 5 V body rail, motor control, and encoder signals through the rotating base joint |

### Camera Specifications

| Property | Value |
|---|---|
| Sensor | OV5640, 5 MP |
| Interface | 8-bit parallel DVP to ESP32-S3 camera peripheral; I2C (SCCB) for autofocus control |
| Resolution | VGA (640×480) or higher via `esp32-camera`; JPEG output |
| Horizontal FOV | 72° (non-distorting lens) |
| XCLK | Internal 24 MHz oscillator on breakout (enable via jumper) |
| Capture pipeline | `esp32-camera` on ESP32-S3; JPEG frames forwarded to Jetson over USB serial |

### PCA9685 Wiring (Planned)

| Signal | Jetson 40-pin header pins |
|---|---|
| I2C SDA | Pin 3 |
| I2C SCL | Pin 5 |
| VCC (logic) | Pin 1 (3.3 V) |
| GND | Any GND pin |
| V+ (servo power) | Pins 2 or 4 (5 V, shared rail — see Power section) |

### Servo PWM Parameters

| Parameter | Value |
|---|---|
| PWM frequency | 50 Hz |
| Minimum pulse width | 500 µs |
| Maximum pulse width | 2400 µs |
| Channel 0 | Servo L (left push/pull rod) |
| Channel 1 | Servo R (right push/pull rod) |

### Neck Mechanism

Three-arm differential design. The head is connected to the neck at three
points: one passive rear pivot and two servo-driven push/pull rods at the
front.

**Connection points**

| Point | Type | Position | Notes |
|---|---|---|---|
| Back pivot | Ball joint (M3 rod-end or printed socket) | Rear centre of head, at head CG height | Passive — provides the reaction point. Ball joint allows small compliance to prevent binding during combined pitch+roll. |
| Left rod | M3 threaded rod with ball-link ends | Front-left of head, 35 mm left of centreline | Driven by Servo L. |
| Right rod | M3 threaded rod with ball-link ends | Front-right of head, 35 mm right of centreline | Driven by Servo R. |

**Geometry constraints**

| Dimension | Value | Notes |
|---|---|---|
| Rod attachment width | 35 mm (centre-to-centre) | Narrower = more pitch authority relative to roll. Calibrate after first print. |
| Back pivot height | At head CG | Head CG must be measured with all components installed. |
| Rod angle at neutral | ~perpendicular to head front face | Maximises mechanical advantage at the midpoint of travel. |
| Servo horn radius | 15 mm (starting point) | Adjust to tune travel range vs. torque. |
| Hard stops | ±32° pitch, ±17° roll | 2° mechanical margin beyond software limits. |

**Servo command mixing**

Pitch (nod) and roll (head-cock) are computed from the two servo positions:

```
pitch = (servo_L + servo_R) / 2
roll  = (servo_L - servo_R) / 2
```

To command a desired pitch and roll:

```
servo_L = pitch_cmd + roll_cmd
servo_R = pitch_cmd - roll_cmd
```

Both servo outputs are clamped to hardware travel limits before being written
to the PCA9685. During normal tracking, `roll_cmd = 0` and both servos move
identically.

**Servo mounting**

Both servos mount at the base of the neck (body side), not inside the head.
This keeps the head's moment of inertia low for faster PID response. The
push/pull rods run up through or alongside the neck tube to the head
attachment points.

---

## Range of Motion (Planned)

### Angle Limits

| Axis | Actuator | Command | Range | Hard stops |
|---|---|---|---|---|
| Pitch (head nod) | Servo L + Servo R together | `pitch_cmd` | ±30° | Yes, mechanical at ±32° |
| Roll (head cock) | Servo L vs Servo R differential | `roll_cmd` | ±15° | Yes, mechanical at ±17° |
| Yaw (base) | N20 + DRV8833 | base yaw command | 360° continuous | None — encoder-tracked in software |

The head has no pan axis. All horizontal tracking is handled by base yaw.
Positive pitch = head tips up. Positive roll = head cocks right. Positive yaw =
clockwise viewed from above. Pitch and roll are clamped in software before
servo mixing; hard stops are a backup.

### Named Positions

| Position | Yaw | Pitch | Roll | Description |
|---|---|---|---|---|
| `home` | current | 0° | 0° | Head level, centred. Base holds position. |
| `rest` | current | -5° | 0° | Slight downward pitch toward seated user. Default between conversations. |
| `scan_start` | -45° | -5° | 0° | Base yaw at left edge of scan sweep. Head at rest pitch. |
| `scan_end` | +45° | -5° | 0° | Base yaw at right edge of scan sweep. Head at rest pitch. |

Base yaw is not reset on idle — it holds the last oriented position.

### Scan Pattern

On wake the base sweeps from -45° to +45° yaw at ~20°/s while the head holds
rest pitch (-5° pitch, 0° roll). The sweep aborts as soon as a face is detected
or after 2.5 s. If no face is found within the cap, listening begins at whatever
yaw position the sweep reached.

The head servos do not move during the scan sweep — only the base rotates.

---

## Vision Service Design (Planned)

> This section describes the intended design of `vision_service.py`, a
> background process that will run on the Jetson. No code is written yet.

### Responsibilities

The vision service will:

1. Read incoming JPEG frames from the USB serial port (forwarded by the ESP32-S3 from the OV5640).
2. Run MediaPipe FaceDetector (full-range model, `model_selection=1`) on each
   frame at approximately 20–30 fps.
3. Maintain the current tracking state (face centroid, confidence, pitch, roll,
   yaw, servo L/R angles).
4. Run a PID position-control loop to convert face centroid error into pitch
   and yaw commands; apply servo mixing for servo L and servo R.
5. Write servo L/R pulse widths to the PCA9685 over I2C; write base yaw
   commands to the DRV8833 via PCA9685 channels 2–3.
6. Serve a small localhost HTTP API so the bridge can query state and issue
   control commands.

### PID Control Loop Design

The face centroid is expressed in normalized image coordinates where (0.5, 0.5)
is the center of the frame.

| Variable | Definition |
|---|---|
| `error_x` | `centroid_x − 0.5` (positive = face is right of centre) |
| `error_y` | `centroid_y − 0.5` (positive = face is below centre) |
| `yaw_correction` | `PID(error_x) × fov_h` — sent to base yaw motor |
| `pitch_correction` | `PID(error_y) × fov_v` — applied via servo mixing |

`fov_h` and `fov_v` are the camera's horizontal and vertical field of view in
degrees; measure per chosen webcam model.

Servo mixing applies pitch correction with roll held at zero during tracking:

```
servo_L_cmd = pitch_correction + 0   (roll = 0 during tracking)
servo_R_cmd = pitch_correction - 0
```

All commanded values are clamped to hardware limits before output. When face
detection confidence falls below threshold or no face is present, the PID
integrators are frozen and all actuators hold their last commanded position.

### Tracking Strategy

The head has no pan axis. Horizontal and vertical tracking use separate
actuators with no interaction between loops:

| Axis | Actuator | PID input | Speed |
|---|---|---|---|
| Horizontal | Base yaw (N20 motor) | `error_x` | ~20–40°/s |
| Vertical | Head pitch (differential servo) | `error_y` | ~60°/s max slew |
| Roll | Differential servo | Not used during tracking (roll = 0) | — |

Because the base handles all horizontal correction and the head handles all
vertical correction, there is no two-stage interaction or cross-axis dependency
to manage. Each PID loop is independent.

### Localhost HTTP API

The vision service will expose the following endpoints on localhost (port TBD):

#### GET /face

Returns current face tracking state.

| Field | Type | Description |
|---|---|---|
| `x` | float | Normalized face centroid X (0.0–1.0) |
| `y` | float | Normalized face centroid Y (0.0–1.0) |
| `confidence` | float | Face detection confidence (0.0–1.0) |
| `pitch_deg` | float | Current head pitch command in degrees |
| `roll_deg` | float | Current head roll command in degrees |
| `yaw_deg` | float | Current base yaw position in degrees (encoder-derived) |
| `servo_l_deg` | float | Current Servo L pulse position in degrees |
| `servo_r_deg` | float | Current Servo R pulse position in degrees |

Example: `{"x":0.52,"y":0.41,"confidence":0.94,"pitch_deg":-4.1,"roll_deg":0.0,"yaw_deg":12.3,"servo_l_deg":-4.1,"servo_r_deg":-4.1}`

#### GET /frame

Returns the latest JPEG frame received from the ESP32-S3. Used by the bridge when a
visual query is needed.

Response: `image/jpeg` binary body.

#### POST /track

Enables or disables servo output from the PID loop.

Request body:

| Field | Type | Description |
|---|---|---|
| `enabled` | bool | `true` to start tracking, `false` to hold position |

When disabled, the servos hold the last commanded position. The `home` and
`rest` commands below work regardless of tracking state.

#### POST /home

Drives head to pitch=0°, roll=0° (servo_L=0°, servo_R=0°) immediately.
Ignores tracking state.

No request body required.

#### POST /rest

Drives head to pitch=-5°, roll=0° (servo_L=-5°, servo_R=-5°) immediately.
Ignores tracking state.

No request body required.

#### GET /health

Liveness check. Returns 200 OK if the capture pipeline is running and the
PCA9685 is reachable.

---

## Robot State Additions (Planned)

Two new state concepts are planned for Milestone 5. They extend the existing
state table in `README.md` without replacing it.

### New State: `scanning`

| Property | Value |
|---|---|
| Trigger | Wake event received |
| Behavior | Base yaw sweeps from -45° to +45° at ~20°/s; head holds rest pitch (-5°, roll 0°) |
| Transition out | Face locked (→ `listening` + tracking active) or 2.5 s timeout (→ `listening`, no lock) |
| LED | Same as `wake_detected` → `listening` — no additional LED pattern needed |
| Duration cap | 2.5 s |

The scanning state runs during the transition from wake detection to listening.
If a face is found within the cap, tracking activates and the conversation
proceeds normally. If no face is found within 2.5 s, listening begins anyway
with the head at whatever position the sweep reached.

### New Mode Flag: `tracking`

Tracking is not a standalone state — it is a concurrent mode flag that can be
active during `listening`, `thinking`, and `speaking` states.

| Property | Value |
|---|---|
| Activated | When a face is locked during scanning |
| Deactivated | When the robot returns to idle |
| Effect | PID loop drives servos each frame to keep face centered |
| LED | No change — underlying conversation state LEDs remain in effect |

Because tracking is a background mode flag rather than a foreground state, no
new LED pattern is needed for it. The head simply moves while the existing
conversation LED patterns play.

---

## Bridge Integration (Planned)

> This section describes planned changes to
> `examples/3bo/bridge/bridge.py` and the generic bridge at
> `examples/voice-bridge/bridge.py`. No code is written yet.

### Vision Service Base URL

The bridge will read a `THREEBO_VISION_URL` environment variable (default:
`http://127.0.0.1:PORT`). All vision API calls go to that base.

### Wake Event Handler (Planned)

When the bridge receives a wake event:

1. Call `POST /vision/track` with `{"enabled": true}`.
2. Begin the servo scan sweep by driving servos to `scan_start` position and
   issuing incremental angle commands toward `scan_end` at 20°/s.
3. Poll `GET /vision/face` each sweep step. If `confidence` exceeds a threshold
   (TBD, e.g. 0.85), stop sweep and let PID loop take over.
4. After lock or 2.5 s timeout, transition to `listening`.

### Idle Return Handler (Planned)

When the bridge returns to idle after a conversation:

1. Call `POST /vision/rest` to return the head to the rest pose.
2. Call `POST /vision/track` with `{"enabled": false}`.

### Visual Keyword Detection (Planned)

Before forwarding a transcript to Arbiter, the bridge will run a
`needs_vision(transcript)` check.

#### Trigger Keywords

| Keyword or phrase |
|---|
| see |
| seeing |
| look |
| looking |
| show |
| in front of you |
| around you |
| what is that |
| describe |
| notice |

#### Visual Query Pipeline

When `needs_vision` returns true:

1. Call `GET /vision/frame` to retrieve the latest JPEG.
2. Base64-encode the frame.
3. Call the Ollama `/api/generate` endpoint with model `moondream` and the
   encoded frame as the image input.
4. Use the prompt: `"Describe what you see concisely in two sentences."`
5. Prepend the model's response to the Arbiter message as:
   `[Visual context: <description>]`
6. Route the message to the cloud agent regardless of complexity classification
   (visual context requires a full model response; the local fast-path agent
   should not receive image-derived context).

#### moondream2 Model Details

| Property | Value |
|---|---|
| Model | moondream2 |
| Size | ~1.6 B parameters |
| Ollama name | `moondream` |
| Pull command | `ollama pull moondream` |
| API | Standard Ollama `/api/generate` with base64 image field |
| Prompt | "Describe what you see concisely in two sentences." |

---

## Latency Notes (Planned)

These are expected latency figures based on hardware specifications. Actual
values will need to be measured during Milestone 5 integration.

| Operation | Expected latency |
|---|---|
| Face detection loop | ~33 ms per frame at ~30 fps |
| PID update | Each frame (~33 ms interval) |
| moondream2 frame query on Jetson Orin Nano | ~1.5–3 s |
| MG90S servo slew (rest to typical face angle ~30°) | ~50 ms |
| Scan to face lock (face within ±45°, 20°/s sweep) | 0–4.5 s |
| Scan timeout fallback | 2.5 s cap |

The 2.5 s scan cap keeps wake-to-listening latency bounded at a level that
feels acceptable even when no face is present. The VLM query latency (1.5–3 s)
is additive to the normal STT and Arbiter latency for visual queries; that
budget should be communicated to users if possible (e.g. an extended thinking
LED phase).

---

## Power Notes (Planned)

### Servo Current Budget

| Condition | Current per servo | Total (2 servos) |
|---|---|---|
| Idle / holding position | ~50 mA | ~100 mA |
| Active movement | ~150 mA (typical) | ~300 mA |
| Stall (hard limit) | ~250 mA | ~500 mA |

### Supply Rail Plan

| Rail | Source | Load |
|---|---|---|
| Servo V+ | Jetson 40-pin 5 V (pins 2 and 4) | PCA9685 V+ → Servo L, Servo R |
| Motor VM | Same 5 V rail → DRV8833 VM | N20 base motor (100–300 mA typical) |
| PCA9685 logic VCC | Jetson 40-pin 3.3 V (pin 1) | PCA9685 logic only |
| Encoder VCC | Base 3.3 V rail | N20 encoder logic |
| LED and audio 5 V | Existing USB/Nano body rail | NeoPixel, MAX98357A amp |

Peak draw with both servos slewing and base motor running: ~700 mA on the 5 V
rail. The Jetson 40-pin 5 V header is rated ~3 A — sufficient with margin.
Add bulk capacitance (220–470 µF) near both the PCA9685 V+ terminal and the
DRV8833 VM pin to absorb simultaneous inrush from servos and motor start.

The Jetson 40-pin 5 V rail (pins 2 and 4) can supply up to approximately 3 A,
which comfortably covers two MG90S servos at peak draw with margin remaining
for other 5 V peripherals.

### Rail Isolation

Keep servo V+ on the PCA9685 board separate from the logic body rail serving
LEDs and audio. Servo PWM noise and inrush current during slew should not
affect the audio amplifier or NeoPixel power path. Add bulk capacitance (100–
470 µF) near the PCA9685 V+ terminal to absorb slew inrush.

Do not exceed the Jetson header 5 V current rating. If a future build adds more
servos, a dedicated 5 V regulator fed from the main battery rail is the
recommended path — not additional draws on the header pins.

---

## Milestone Placement

| Milestone | Status | Scope |
|---|---|---|
| Milestone 1 | Planned | Software loop: Jetson, Arbiter, STT, TTS, bridge |
| Milestone 2 | Planned | Audio and LED loop: I2S mic, speaker, NeoPixel |
| Milestone 3 | Planned | Wake word: on-device detection, VAD, mute switch |
| Milestone 4 | Planned | Product hardening: pairing, persistence, OTA, recovery |
| **Milestone 5** | **This document** | **Vision and head-tracking: camera, servos, VLM queries** |

### v1 Prototype Constraints

The v1 prototype (Milestones 1–4) has no camera, no PCA9685, and no servos.
None of the hardware described in this document is installed in v1.

The neck bracket design should include servo pocket cutouts and mounting holes
sized for MG90S servos so that Milestone 5 hardware can be installed without
a major mechanical rebuild. Pockets should be left empty in v1, with the servo
wire channels sealed against debris.

The vision service (`vision_service.py`) does not run in v1. Its dependencies
(MediaPipe, OpenCV, `smbus2`, PCA9685 library) are not installed in v1.

---

*Last updated: 2026-06-12. This is a design document. All content describes
planned future work for Milestone 5 and does not reflect the current state of
the 3bo prototype.*
