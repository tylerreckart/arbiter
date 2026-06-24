# 3bo circuit design

This is the first bench circuit for 3bo: Jetson Orin local brain, Nano ESP32
body controller, ICS-43434 I2S microphone, MAX98357A I2S amplifier, 8 ohm
breadboard speaker, RGBW NeoPixel stick through a 74AHCT/74HCT level shifter,
and a physical mute switch.

## Power topology

Use Jetson-powered body-controller wiring for the preferred bench build:

```text
Jetson Orin brain
  -> vendor-recommended Jetson power supply
  -> Ethernet or supported Wi-Fi adapter for development network
  -> USB host port
      -> USB-C data cable
          -> Arduino Nano ESP32 power + serial data

Nano USB/VBUS or verified Jetson-powered 5 V body rail
  -> MAX98357A VIN
  -> NeoPixel +5V
  -> 74AHCT/74HCT VCC

Nano 3.3 V rail
  -> hard-mute switch or load switch
  -> ICS-43434 VIN

All Nano/body electronics grounds tied together through the USB/common ground.
```

For portable battery testing, keep the battery concerned only with the Jetson:

```text
4S LiPo/Li-ion pack
  -> fuse near battery positive
  -> hard low-voltage cutoff / protected BMS output
  -> DC-rated main switch
      -> 19 V Jetson regulator
          -> branch fuse/protection
          -> Jetson 5.5 mm x 2.5 mm center-positive input

Jetson USB host port
  -> USB-C data cable
      -> Nano ESP32 power + serial data
      -> low-current body electronics

If measured LED/audio current exceeds the safe Jetson USB/Nano 5 V budget, add
a Jetson-powered USB hub or current-limited 5 V accessory rail. Do not add a
separate battery-fed Nano/body branch unless the power design is reopened.
```

The Jetson is the only battery-fed high-current compute load. Use the
vendor-recommended Jetson supply first. For a final single-input enclosure, add
a dedicated regulator matched to the exact Jetson carrier board input. Keep the
Nano on Jetson USB power, and treat LED/audio 5 V as a measured Jetson-powered
USB/accessory load.

The Nano and Jetson share ground through USB. Do not also power the Nano from
`VIN` while it is connected to the Jetson USB host unless the exact board power
path has been reviewed and backfeed protection is verified. The preferred 3bo
prototype uses one Nano power source: Jetson USB.

The Adafruit product 184 supply is a low-dropout linear regulator. It is no
longer part of the runtime power path. Keep it for bench measurement or an
isolated low-current peripheral experiment, and do not connect it in parallel
with the Jetson USB-powered Nano/body rail.

The simplest 3.3 V source is the Nano ESP32's `3V3` pin after the Nano is
powered from Jetson USB. The ICS-43434 microphone draws very little current, so
this is a reasonable low-current load. If you use a separate 3.3 V rail, use it
for peripherals only; do not backfeed the Nano `3V3` pin unless the board
documentation explicitly allows that power-in path. Route whichever 3.3 V mic
source you choose through the hard-mute switch or a load-switch module before it
reaches the microphone.

## Proposed Nano ESP32 pins

The ESP32-S3 can route I2S signals flexibly, so these pins are chosen for a
clean breadboard layout rather than because I2S is fixed to them.

| Function | Nano pin | Direction | Notes |
| --- | --- | --- | --- |
| I2S bit clock | `D2` | output | Shared by mic and amp. |
| I2S word select / LR clock | `D3` | output | Shared by mic and amp. |
| I2S mic data | `D4` | input | From ICS-43434 `DOUT` / `SD`. |
| I2S amp data | `D5` | output | To MAX98357A `DIN`. |
| NeoPixel data | `D6` | output | Goes through 74AHCT/74HCT before NeoPixel `DIN`. |
| Mute sense | `D7` | input | Firmware-visible mute state. Use internal pullup or external 10 kOhm pullup. Switch pulls to GND. |
| Amp shutdown | optional `D8` | output | Optional. Tie amp `SD`/shutdown high if not firmware-controlled. |

Avoid `D0`/`D1` for the first build so serial/debug behavior stays boring.

## Wiring table

### Power and ground

| From | To | Notes |
| --- | --- | --- |
| Jetson power supply or 19 V battery regulator | Jetson Orin carrier power input | Use the supply or regulator recommended for the exact carrier board. |
| Jetson network | Ethernet or supported Wi-Fi adapter | Use Ethernet for first bring-up if available. |
| Jetson USB host | Nano ESP32 USB-C | Runtime power and serial data for the Nano. Use a short data-capable cable. |
| Nano ESP32 `GND` | Breadboard GND rail | Required for every body signal; ground is common through USB. |
| Verified USB/VBUS 5 V or Jetson-powered accessory rail | Breadboard 5 V load rail | Feeds amp, NeoPixel, and level shifter. Verify current budget before connecting modules. |
| 5 V load rail `-` | Breadboard GND rail | Common ground for 5 V loads. |
| Nano ESP32 `3V3` | Hard-mute switch/load-switch input | Low-current 3.3 V mic rail for bench builds. Do not backfeed this pin. |
| Hard-mute switch/load-switch output | ICS-43434 `3V` / `VIN` | Microphone is physically unpowered when muted. |
| 470-1000 uF capacitor `+` | Breadboard 5 V rail | Place near NeoPixel/amp. |
| 470-1000 uF capacitor `-` | Breadboard GND rail | Observe polarity. |

### ICS-43434 I2S microphone, product 6049

| ICS-43434 pin | Connects to | Notes |
| --- | --- | --- |
| `3V` / `VIN` | Hard-mute switch/load-switch output | Do not power from 5 V. This rail must be off when muted. |
| `GND` | GND rail | Common ground. |
| `BCLK` / `SCK` | Nano `D2` | I2S bit clock. |
| `WS` / `LRCL` | Nano `D3` | I2S word select. |
| `DOUT` / `SD` | Nano `D4` | I2S microphone data into Nano. |
| `SEL` / `L/R` | GND | Select one channel. Use 3V3 instead if firmware expects the other channel. |

Mount the mic so the port faces outward and is not pressed against the table or
enclosure wall.

### MAX98357A I2S amplifier, product 3006

| MAX98357A pin | Connects to | Notes |
| --- | --- | --- |
| `VIN` | 5 V rail | Amp delivers ~1.8 W into 8 Ω on 5 V — stay below the 1 W speaker rating. |
| `GND` | GND rail | Common ground. |
| `BCLK` | Nano `D2` | Shared I2S bit clock. |
| `LRC` / `LRCLK` | Nano `D3` | Shared I2S word select. |
| `DIN` | Nano `D5` | I2S audio data from Nano. |
| `GAIN` | leave default | Default gain is fine for bring-up. |
| `SD` / shutdown | 3V3 or optional Nano `D8` | Tie enabled for first test, or control from firmware. If tied high, use 3.3 V logic. |
| `+` speaker output | Speaker `+` | Bridge-tied output. Do not connect to GND. |
| `-` speaker output | Speaker `-` | Bridge-tied output. Do not connect to GND. |

The MAX98357A delivers approximately 1.8 W into 8 Ω on a 5 V supply. The
selected speaker (product 1313) is rated 1 W, so keep software volume moderate.

### Speaker, product 1313

3" diameter, 8 Ω, 1 W. Four mounting tabs at 60 mm spacing.

| Speaker pin | Connects to | Notes |
| --- | --- | --- |
| One speaker lead | MAX98357A speaker `+` | Polarity is not critical for a single speaker. |
| Other speaker lead | MAX98357A speaker `-` | Do not connect either speaker lead to GND. |

### 74AHCT/74HCT level shifter for NeoPixel data

Use one channel of the 74AHCT/74HCT part. Exact pin names vary by package, but
the logic is the same.

| Level shifter pin | Connects to | Notes |
| --- | --- | --- |
| `VCC` | 5 V rail | Makes the output a 5 V logic signal. |
| `GND` | GND rail | Common ground. |
| `A1` / input | Nano `D6` | 3.3 V NeoPixel data from Nano. |
| `Y1` / output | 330-470 ohm resistor | 5 V data toward NeoPixel. |
| `OE` / output enable | active state for your part | Tie to the enabled state; many 74AHCT parts use active-low `OE`, so tie `OE` to GND. |
| `DIR` | fixed direction, if present | Tie for A-to-Y direction on bidirectional parts. Not present on simple buffers. |
| Unused inputs | GND or defined level | Do not leave CMOS inputs floating. |

### NeoPixel Jewel RGBW, product 2858

7 pixels: index 0 is the centre LED; indices 1–6 are the outer ring. Firmware
addresses them individually — ring-chase animations rotate through indices 1–6,
with the centre pixel held at a dim accent colour.

| NeoPixel pin | Connects to | Notes |
| --- | --- | --- |
| `PWR` / `+` | 5 V rail | Cap brightness in firmware. |
| `GND` / `-` | GND rail | Common ground. |
| `DIN` | 330-470 ohm resistor from level shifter output | Use the `DIN` pad, not `DOUT`. |
| `DOUT` | unconnected | Only used if chaining another Jewel. |

Firmware type constant: `NEO_GRBW + NEO_KHZ800` (same protocol as the former NeoPixel Stick).

### Hard mute switch

Use the switch for two things: physically disable microphone power and tell
firmware the muted state. A DPDT switch is the simplest prototype part because
one pole can switch the microphone rail while the other drives the Nano input.
An SPST switch is acceptable only if it controls a load-switch enable and the
firmware-visible state comes from the same hard-mute signal.

| Switch pin | Connects to | Notes |
| --- | --- | --- |
| Pole A common | ICS-43434 `3V` / `VIN` | Switched microphone power. |
| Pole A unmuted throw | Nano `3V3` or separate 3.3 V mic rail | Mic receives power only when unmuted. |
| Pole A muted throw | unconnected | Leaves mic unpowered. |
| Pole B common | Nano `D7` | Configure as `INPUT_PULLUP`. |
| Pole B muted throw | GND rail | Switch closed means muted/active-low. |
| Pole B unmuted throw | unconnected | Internal pullup reads unmuted. |

In firmware, treat `D7 == LOW` as muted. When muted, stop wake-word detection,
ignore audio frames, and show the muted LED state. The privacy guarantee comes
from the microphone power being removed, not from this firmware branch.

## Bring-up order

1. Bring up the Jetson Orin separately with its vendor-recommended power supply,
   cooling, storage, network, and SSH access.
2. Build and run `arbiter --api` and the 3bo bridge on the Jetson before
   connecting the robot body electronics.
3. Connect the Nano to a Jetson USB host port with a data-capable USB-C cable.
   Confirm the Jetson sees the Nano serial device before connecting amp/LED
   loads.
4. Verify the available 5 V body rail and current budget before connecting the
   MAX98357A and NeoPixel stick. Keep LED brightness and speaker volume low.
5. Verify the hard-mute switch removes microphone 3.3 V in the muted position,
   then connect the ICS-43434 microphone.
6. Test NeoPixel output through the 74AHCT/74HCT at low brightness.
7. Test the MAX98357A with a very quiet generated tone or WAV playback.
8. Test the ICS-43434 by printing audio levels or recording a short buffer.
9. Confirm the firmware can run I2S input and I2S output together. If shared
   BCLK/WS is problematic, split mic and amp onto separate I2S peripherals or
   disable playback while listening.
10. Verify both mute paths: microphone 3.3 V physically drops to 0 V and the
   firmware sees the expected active-low state.
11. Point the Nano firmware at the Jetson bridge URL and run the full loop:
   wake/listen LED states, local recording, bridge upload, local STT, Arbiter,
   TTS playback.

## First-test firmware constants

```cpp
constexpr int PIN_I2S_BCLK = D2;
constexpr int PIN_I2S_WS   = D3;
constexpr int PIN_I2S_MIC  = D4;
constexpr int PIN_I2S_AMP  = D5;
constexpr int PIN_PIXELS   = D6;
constexpr int PIN_MUTE     = D7;
constexpr int PIN_AMP_SD   = D8; // optional

constexpr int PIXEL_COUNT = 8;
constexpr bool MUTE_ACTIVE_LOW = true;
```

These constants may need adjustment depending on the Arduino Nano ESP32 core
and I2S library used. Keep the circuit table and firmware constants in sync.

## PCA9685 servo driver (Milestone 5)

This section documents the servo driver wiring for Milestone 5 (vision and head tracking). It is not needed for the v1 voice prototype.

### Jetson 40-pin header to PCA9685

| Jetson 40-pin header | PCA9685 pin | Notes |
| --- | --- | --- |
| Pin 1 (3.3V) | VCC | Logic supply. Do not use 5V for VCC; PCA9685 logic is 3.3V or 5V compatible but 3.3V matches Jetson GPIO. |
| Pin 2 or 4 (5V) | V+ | Servo power rail. Two MG90S servos draw up to ~500 mA peak. |
| Pin 3 (SDA, I2C1) | SDA | I2C data. |
| Pin 5 (SCL, I2C1) | SCL | I2C clock. |
| Pin 6 or 9 (GND) | GND | Common ground. |

Default I2C address: 0x40. Confirm with `i2cdetect -y 1` on the Jetson.

### PCA9685 to servos

| PCA9685 channel | Servo | Purpose | Pulse range |
| --- | --- | --- | --- |
| Channel 0 | MG90S Servo L | Left push/pull rod. Pitch+roll mixed output. | 500–2400 µs at 50 Hz |
| Channel 1 | MG90S Servo R | Right push/pull rod. Pitch−roll mixed output. | 500–2400 µs at 50 Hz |

Each servo connector (signal, VCC/+, GND) plugs directly into the corresponding PCA9685 channel header. Signal = PWM output. VCC = from V+ rail. GND = common.

MG90S centre position (0°) ≈ 1500 µs. At 50 Hz, one period = 20 ms. Approximate endpoints: +30° ≈ 1800 µs, −30° ≈ 1200 µs. Calibrate by observing servo behaviour — endpoints vary by unit. The software sends mixed pitch/roll commands; calibrate each servo independently against the physical rod geometry.

Do not power servos from the Nano ESP32 or the body 5V rail. The PCA9685 V+ draws from the Jetson 40-pin 5V pins (pins 2/4), which are rated up to 3A on the Orin Nano dev kit. Keep servo V+ wiring short and use 22 AWG or heavier wire for the servo power run.

## N20 base motor and slip ring (Milestone 5)

This section documents the base rotation motor, H-bridge driver, encoder, and
slip ring wiring. Not needed for v1.

### Slip ring

The slip ring sits at the center of the base bearing and passes all power and
signals between the stationary base and the rotating body. Use a 12-wire
capsule slip ring rated ≥ 2 A per circuit.

| Slip ring circuit | Carries | From → To |
| --- | --- | --- |
| 1–2 | 19 V, 3 A (Jetson supply) | Base regulator → body Jetson input |
| 3–4 | 5 V, 1.5 A (body rail) | Base 5 V regulator → body rail |
| 5–6 | GND | Common ground |
| 7 | Motor IN1 (PWM) | PCA9685 channel 2 → DRV8833 IN1 |
| 8 | Motor IN2 (PWM) | PCA9685 channel 3 → DRV8833 IN2 |
| 9 | Encoder A | N20 encoder A → Jetson GPIO |
| 10 | Encoder B | N20 encoder B → Jetson GPIO |
| 11–12 | Spare | Reserved |

Keep signal wires (IN1, IN2, encoder A/B) away from the high-current 19 V and
motor power wires within the slip ring bundle. Twist encoder pairs if possible.

### DRV8833 motor driver

The DRV8833 lives in the base tier alongside the N20. It is powered from the
base 5 V rail and controlled by PWM signals from PCA9685 channels 2 and 3
(carried through the slip ring).

| DRV8833 pin | Connects to | Notes |
| --- | --- | --- |
| VM | 5 V rail (base) | Motor power. N20 at 6 V is fine on a clean 5 V rail; rated max is 10.8 V. |
| GND | GND | Common ground. |
| IN1 | PCA9685 channel 2 (via slip ring) | PWM forward signal. |
| IN2 | PCA9685 channel 3 (via slip ring) | PWM reverse signal. |
| OUT1 | N20 motor terminal A | H-bridge output. |
| OUT2 | N20 motor terminal B | H-bridge output. |
| SLP (sleep) | 3.3 V or 5 V (tied high) | Pull high to enable driver. Can be tied to a Jetson GPIO for software sleep if needed. |
| FLT (fault) | Jetson GPIO (optional) | Open-drain fault indicator; pull up to 3.3 V and read on a Jetson GPIO if overcurrent monitoring is wanted. |

### N20 encoder

The N20 quadrature magnetic encoder outputs two square wave channels (A and B)
90° out of phase. Connect to two Jetson GPIO interrupt-capable pins.

| Encoder wire | Connects to | Notes |
| --- | --- | --- |
| VCC | 3.3 V (base rail) | Encoder logic supply. Verify encoder VCC requirement; most N20 encoders accept 3.3–5 V. |
| GND | GND | Common ground. |
| A | Jetson GPIO (interrupt pin, via slip ring) | Channel A quadrature output. |
| B | Jetson GPIO (interrupt pin, via slip ring) | Channel B quadrature output. |

Encoder count direction (CW vs CCW) depends on motor orientation. Determine
the positive direction during first bring-up and set the sign convention in
software.

### PCA9685 channel allocation (updated)

| Channel | Use | Signal type |
| --- | --- | --- |
| 0 | Pan servo | 50 Hz servo PWM |
| 1 | Tilt servo | 50 Hz servo PWM |
| 2 | Base motor IN1 (DRV8833) | PWM (0–100 % duty) |
| 3 | Base motor IN2 (DRV8833) | PWM (0–100 % duty) |
| 4–15 | Available | — |

Drive the base motor by setting one channel to the desired duty cycle and the
other to 0 (or both to 0 to coast, both to 100 % to brake). Do not set both
channels to a non-zero duty simultaneously.

## OV5640 camera (Milestone 5)

The head camera is an Adafruit OV5640 Camera Breakout — 72° Lens with Autofocus
(product 5945). It connects to the Arduino Nano ESP32 via 8-bit parallel DVP.
On the prototype, wire the PiCowbell breakout's header pins to the Nano ESP32
with jumper wires. On the final build, route signals through the custom carrier
board.

### XCLK

The PiCowbell breakout has an onboard 24 MHz oscillator. Enable it via the
board's XCLK jumper so the ESP32-S3 does not need to generate the clock.

### DVP signal wiring (prototype — jumper wires)

Consult the PiCowbell schematic for the Pico GPIO numbers that carry each
signal, then map them to any available ESP32-S3 GPIO pins. The `esp32-camera`
configuration struct assigns signals by GPIO number, so the mapping is flexible.
Keep data wires short to minimize parallel bus noise.

| OV5640 signal | Direction | Notes |
| --- | --- | --- |
| D0–D7 | input to ESP32-S3 | 8-bit pixel data |
| VSYNC | input to ESP32-S3 | Frame sync |
| HREF | input to ESP32-S3 | Line sync |
| PCLK | input to ESP32-S3 | Pixel clock |
| SIOD | bidirectional | I2C data for autofocus (SCCB) |
| SIOC | output from ESP32-S3 | I2C clock for autofocus (SCCB) |
| RESET | output from ESP32-S3 | Active-low sensor reset |
| PWDN | output from ESP32-S3 | Active-high power-down |
| 3.3 V | power | From Nano ESP32 `3V3` or shared 3.3 V rail |
| GND | ground | Common ground |

### Library

Use the `esp32-camera` component (Espressif). OV5640 is a supported sensor.
Configure the pin assignments in the camera config struct to match the chosen
GPIO mapping. Set output format to JPEG and target VGA (640×480) or higher
depending on bandwidth and latency measurements.

The Adafruit PiCowbell library is RP2040-specific and is not used here.

## Wake-word audio format

ESP-SR WakeNet expects 16 kHz mono signed 16-bit audio. The ICS-43434 outputs
24-bit I2S audio, so firmware must convert the incoming I2S samples into the
format expected by the wake-word engine before inference.
