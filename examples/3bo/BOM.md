# 3bo bill of materials

This BOM targets the first 3bo prototype: Jetson Orin local brain, Arduino Nano
ESP32 body controller, wake-word listening, I2S microphone input, I2S speaker
output, status LEDs, physical mute, and a consistent bench power setup.

Prices vary by vendor and region, so the cost column is a planning estimate,
not a purchasing quote.

Power note: the recommended mobile prototype uses one 4S battery pack feeding
the Jetson's 19 V rail. The Nano ESP32 is powered by, and communicates over,
USB-C from a Jetson USB host port. Keep the LED/audio loads small enough for
the verified USB/body 5 V budget, or add a Jetson-powered USB hub/accessory
5 V rail after measuring current.

## Core electronics

| Qty | Item                                     | Suggested spec/example                                                            | Est. USD | Notes                                                                                                                                                                                                                                                                 |
| --- | ---------------------------------------- | --------------------------------------------------------------------------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | Local Linux/AI brain                     | NVIDIA Jetson Orin Nano Super Developer Kit                                       | 249+     | Runs 3bo bridge, Arbiter daemon, local STT/TTS, logs, and model storage.                                                                                                                                                                                              |
| 1   | Jetson storage                           | microSD card or NVMe SSD supported by the carrier                                 | 15-80    | Prefer NVMe if the carrier/setup supports it; local STT models and logs benefit from fast storage.                                                                                                                                                                    |
| 1   | Jetson bench power supply                | Vendor-recommended supply for the exact Jetson carrier board                      | varies   | Use this for bring-up before moving to battery power.                                                                                                                                                                                                                 |
| 1   | Jetson cooling — standard                | Vendor-supplied active cooler (ships with dev kit)                                | included | Required for bench bring-up. Tallest option (\~35 mm above module).                                                                                                                                                                                                   |
| 1   | Jetson cooling — low-profile             | Thin copper/aluminium heatsink plate (≤8 mm) + Noctua A4x10 FLX fan (40×40×10 mm) | 20–35    | Saves \~18–20 mm of body height vs the stock cooler. Requires ventilation slots in the body enclosure (one inlet, one outlet). Test sustained STT+TTS+vision load with `tegrastats` — throttle above 70 °C indicates a need for a thicker heatsink or better airflow. |
| 1   | Microcontroller                          | Arduino Nano ESP32                                                                | 25-35    | Main controller. ESP32-S3, 3.3 V I/O, USB-C. Powered by the Jetson USB host link in the wired build.                                                                                                                                                                  |
| 1   | I2S MEMS microphone                      | Adafruit ICS-43434 I2S MEMS microphone breakout, product 6049                     | 5        | Digital mono microphone. Power from 3.3 V; not for 5 V logic.                                                                                                                                                                                                         |
| 1   | I2S audio amplifier                      | Adafruit MAX98357A I2S 3 W class-D amplifier breakout, product 3006               | 6        | Drives the speaker directly from I2S audio. Runs from 2.7-5.5 V and accepts 3.3 V logic.                                                                                                                                                                              |
| 1   | Breadboard speaker                       | Adafruit breadboard-friendly PCB mount mini speaker, 8 ohm 0.2 W, product 1898    | 2        | Quiet first-test speaker. Do not overdrive it with the MAX98357A.                                                                                                                                                                                                     |
| 1   | Final enclosure speaker                  | 8 ohm 1-3 W small speaker                                                         | 3-8      | Optional upgrade once the audio path works. Better suited to spoken responses than the 0.2 W breadboard speaker.                                                                                                                                                      |
| 1   | Addressable LED indicator                | Adafruit NeoPixel Stick, 8 x 5050 RGBW cool white, product 2869                   | 8        | Main 3bo status indicator. 5 V power, one data pin, RGBW library required.                                                                                                                                                                                            |
| 1   | USB-C data cable                         | Short, data-capable USB-C cable from Jetson USB host to Nano ESP32                | 5-15     | Carries power and serial data between Jetson and Nano. Avoid charge-only cables.                                                                                                                                                                                      |
| 1   | USB 5 V breakout or measured VBUS access | USB-C breakout, powered USB hub, or carrier-approved 5 V accessory output         | varies   | Optional only if the NeoPixel/amp need more 5 V current than the Nano exposes safely. Verify current limits before use.                                                                                                                                               |
| 1   | 5 V body rail, optional                  | Jetson-powered USB hub/accessory 5 V rail, current-limited/fused                  | 15-40    | Use only if speaker/LED tests exceed the safe USB/Nano 5 V budget. Not battery-fed separately.                                                                                                                                                                        |
| 1   | Breadboard power supply kit              | Adafruit adjustable breadboard power supply kit, product 184                      | 15       | Bench/test supply or low-current 3.3 V peripheral rail. Do not use as the main 5 V LED/audio rail from 12 V.                                                                                                                                                          |
| 1   | Optional 3.3 V regulator                 | 5 V to 3.3 V regulator module                                                     | 2-6      | Only needed if you want a separate 3.3 V rail instead of using the Nano's low-current `3V3` output for the mic.                                                                                                                                                       |
| 1   | Physical mute switch                     | DPDT toggle/slide switch, or SPST switch plus load-switch control                 | 2-8      | Must remove microphone power, not just signal firmware. Use the second pole or a load-switch enable signal for a hard privacy mute.                                                                                                                                   |
| 1   | Microphone power switch                  | P-channel MOSFET high-side switch or dedicated 3.3 V load switch module           | 2-8      | Disconnects the ICS-43434 3.3 V rail when muted. Firmware also reads mute state, but privacy does not depend on firmware.                                                                                                                                             |
| 1   | Power switch                             | SPST toggle or slide switch rated for expected input current                      | 1-4      | Main Jetson/battery input disconnect for the 19 V supply path.                                                                                                                                                                                                        |

## Portable power subsystem

For the first battery-powered prototype, use an external balance charger and
remove the pack from 3bo for charging. That is simpler and safer than adding
onboard charging before the load profile is measured.

| Qty | Item                         | Suggested spec/example                                                                                           | Est. USD | Notes                                                                                                         |
| --- | ---------------------------- | ---------------------------------------------------------------------------------------------------------------- | -------- | ------------------------------------------------------------------------------------------------------------- |
| 1   | Prototype battery pack       | 4S LiPo, 14.8 V nominal / 16.8 V full, 5000-6000 mAh, XT60 connector                                             | 40-80    | Good first capacity target. Use a hardcase RC pack if the enclosure can fit it.                               |
| 1   | Balance charger              | SkyRC IMAX B6AC V2, HOTA D6 Pro, or equivalent charger that supports 4S LiPo/Li-ion balance charging             | 45-130   | Charge the pack outside the robot for v1. Use the LiPo/Li-ion program that matches the pack chemistry.        |
| 1   | Main fuse holder             | Inline ATO/ATC blade fuse holder, 16 AWG or heavier                                                              | 5-10     | Install as close to the battery positive lead as practical.                                                   |
| 2-3 | Blade fuses                  | 5 A and 10 A ATO/ATC fuses                                                                                       | 3-8      | Start with 5 A for bench tests; size up only after measuring normal current and startup surge.                |
| 1   | Main disconnect switch       | DC-rated rocker/toggle switch, at least 10 A at 24 V DC                                                          | 5-15     | Put after the fuse. Avoid tiny AC-only panel switches for the battery main.                                   |
| 1   | Jetson 19 V regulator        | 4S-compatible buck-boost or boost regulator, 19 V output, at least 3 A continuous, preferably 4-5 A with cooling | 20-60    | Feeds the Jetson DC input. Must pass the regulator acceptance tests below before connecting the Jetson.       |
| 1   | Jetson barrel lead           | 5.5 mm x 2.5 mm center-positive DC plug pigtail                                                                  | 3-8      | Match the exact Jetson carrier board connector. Keep polarity labeled.                                        |
| 1   | Low-voltage cutoff           | 4S LiPo/Li-ion low-voltage disconnect or protected 4S pack/BMS with load cutoff                                  | 15-60    | Required for unattended or enclosed battery use. A buzzer alone is not protection.                            |
| 1   | Low-voltage monitor          | 1S-8S LiPo cell checker/alarm with balance-plug input                                                            | 5-12     | Bench diagnostic only. Set a conservative alarm threshold, but do not rely on it as the cutoff.               |
| 1   | XT60 harness kit             | XT60 male/female pigtails, heat-shrink, 16-18 AWG silicone wire                                                  | 8-20     | Keeps battery wiring serviceable. Use strain relief.                                                          |
| 1   | Final-product battery option | 4S Li-ion pack with integrated 4S BMS, 5000-7000 mAh                                                             | 60-140   | Better fit for an enclosed stationary robot than a hobby LiPo. Still needs a charger matched to the pack/BMS. |

## Passives and wiring

| Qty | Item                            | Suggested spec/example                                             | Est. USD | Notes                                                                                                          |
| --- | ------------------------------- | ------------------------------------------------------------------ | -------- | -------------------------------------------------------------------------------------------------------------- |
| 1   | Bulk capacitor                  | 470-1000 uF electrolytic, 10 V or higher                           | 1-3      | Place across 5 V and GND near the NeoPixel stick/audio amp.                                                    |
| 1   | LED data resistor               | 330-470 ohm resistor                                               | \<1      | Place in series with the WS2812/SK6812 data line.                                                              |
| 1   | Pull resistor kit               | 10 kOhm resistors                                                  | \<1      | Useful for mute switch input if not using internal pullups.                                                    |
| 1   | Breadboard or perfboard         | Solderless breadboard for first test; perfboard for portable build | 5-12     | Move to perfboard once pinout and USB-powered load current are stable.                                         |
| 1   | Jumper wire kit                 | Male/male and male/female Dupont wires                             | 5-10     | Keep I2S wires short during testing.                                                                           |
| 1   | Header pins / screw terminals   | 0.1 inch headers, optional terminal blocks                         | 2-6      | Makes power, speaker, and LED wiring less fragile.                                                             |
| 1   | Spare USB-C cable               | Data-capable                                                       | 3-10     | Programming and serial diagnostics. Charge-only cables will not work for the Jetson serial link.               |
| 1   | Ethernet cable or Wi-Fi adapter | Ethernet for bring-up; supported Wi-Fi for untethered use          | varies   | Jetson Orin Nano dev kits are easiest to bring up over Ethernet. Add wireless only after the base stack works. |

## Enclosure and mechanical

| Qty | Item                    | Suggested spec/example                                   | Est. USD | Notes                                                                     |
| --- | ----------------------- | -------------------------------------------------------- | -------- | ------------------------------------------------------------------------- |
| 1   | Prototype enclosure     | Small plastic project box, 3D print, or laser-cut shell  | 5-20     | Leave openings for mic, speaker, LEDs, USB-C, power, and mute.            |
| 1   | Speaker grille/material | Printed grille, perforated panel, or fabric              | 1-5      | Avoid sealing the speaker behind solid plastic.                           |
| 1   | Diffuser                | Frosted acrylic, translucent print, or silicone diffuser | 2-10     | Makes the LED states feel much more polished.                             |
| 1   | Mounting hardware       | M2/M3 screws, standoffs, adhesive pads                   | 3-10     | Keep the mic mechanically isolated from speaker vibration where possible. |

## Vision and motion subsystem

These parts are planned for **Milestone 5** and are not needed for the initial voice prototype. Design the mechanical neck with servo pockets in v1 so it can be upgraded without a rebuild.

| Qty | Item                       | Suggested spec/example                                                                                   | Est. USD     | Notes                                                                                                                                                                                                                                         |
| --- | -------------------------- | -------------------------------------------------------------------------------------------------------- | ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | Head camera                | Adafruit OV5640 Camera Breakout — 72° Lens with Autofocus, product 5945                                  | 15           | Mounted in robot head. 8-bit parallel DVP to ESP32-S3 camera peripheral via jumper wires on prototype; custom carrier board for final build. 72° non-distorting lens. ESP32-S3 JPEG-compresses frames and forwards to Jetson over USB serial. |
| 1   | Servo driver board         | Adafruit PCA9685 16-channel 12-bit PWM servo driver, product 815                                         | 15           | I2C address 0x40. Ch 0 = Servo L, ch 1 = Servo R, ch 2–3 = base motor DRV8833 IN1/IN2.                                                                                                                                                        |
| 2   | Neck servos                | MG90S metal-gear micro servo, or equivalent 9 g metal-gear servo                                         | 4–8 each     | Differential push/pull rod mechanism. Servo L + Servo R together = pitch (±30°). Servo L − Servo R = roll (±15°). 50 Hz, 500–2400 µs. Metal gears for longevity.                                                                              |
| 1   | Neck mechanism bracket     | Custom 3D-printed neck with rear ball-joint pivot, front rod attachment points, and servo mounts at base | 5–15 (print) | Three-arm differential design: one passive rear pivot (ball joint), two push/pull rods at front (35 mm apart). Servos mount at neck base. Include rod channel and cable bore through neck centre.                                             |
| 1   | N20 gearmotor with encoder | N20 6 V, 100–200 RPM output, with quadrature magnetic encoder (e.g. Pololu #4825 or equivalent)          | 10–18        | Drives base rotation. Encoder provides position feedback for closed-loop yaw control. Choose gear ratio so base turns at \~20–60°/s under load.                                                                                               |
| 1   | Motor driver breakout      | Adafruit DRV8833 dual H-bridge motor driver breakout, product 3297                                       | 5            | Controls N20 direction and speed via two PWM signals from PCA9685 channels 2 and 3. 1.5 A per channel — well within N20 stall current.                                                                                                        |
| 1   | Base bearing               | Lazy Susan bearing, 100–150 mm diameter                                                                  | 5–12         | Supports body weight through full 360° rotation. Choose a ball-bearing type rated for the estimated body load (≥ 2 kg).                                                                                                                       |
| 1   | Base motor mount           | Custom 3D-printed gear/pinion drive bracket                                                              | 3–8 (print)  | Positions the N20 against the bearing rim or inner race. Design drive ratio for target rotation speed. A small pinion on the N20 output shaft driving a printed ring gear on the base is the cleanest approach.                               |
| 1   | Slip ring                  | Capsule slip ring, 12-wire, 2 A per circuit (e.g. Adafruit product 736 or similar)                       | 15–25        | Passes power and signals through the rotating base joint. Routes 19 V Jetson supply, 5 V body rail, GND, motor IN1/IN2, and encoder A/B from the stationary base to the rotating body.                                                        |
| 1   | I2C cable / Dupont wires   | 4-wire female–female Dupont cable, 150–200 mm                                                            | 2–5          | Connects Jetson 40-pin header (I2C pins 3, 5) + 3.3 V (pin 1) + GND to PCA9685 in head tier.                                                                                                                                                  |
| 1   | Servo and motor power      | Wired from regulated 5 V body rail to PCA9685 V+ and DRV8833 VM                                          | 0–3          | PCA9685 V+ + DRV8833 VM share the 5 V rail. Total peak draw (2 servos + N20): \~700 mA. Verify Jetson header or body regulator can supply it.                                                                                                 |

## Optional but useful

| Qty | Item                        | Suggested spec/example                                                | Est. USD | Notes                                                                                                          |
| --- | --------------------------- | --------------------------------------------------------------------- | -------- | -------------------------------------------------------------------------------------------------------------- |
| 1   | Logic level shifter         | 74AHCT/74HCT 3.3 V to 5 V data level shifter                          | 2-6      | Recommended for reliable 5 V NeoPixel data from the Nano ESP32's 3.3 V GPIO.                                   |
| 1   | USB power meter             | Inline USB-C meter                                                    | 8-20     | Useful while estimating Nano ESP32 current during firmware development.                                        |
| 1   | Small development speaker   | Extra 8 ohm speaker                                                   | 2-5      | Handy for testing audio without mounting the final speaker.                                                    |
| 1   | Analog microphone module    | Adafruit MAX9814 electret microphone amplifier with AGC, product 1713 | 8        | Preferred optional ADC-based mic for sound-level experiments when volume varies. Not the wake-word/speech mic. |
| 1   | Analog microphone module    | Adafruit MAX4466 electret microphone amplifier, product 1063          | 7        | Alternate optional ADC-based mic with adjustable gain. Simpler, but no automatic gain control.                 |
| 1   | Rechargeable battery option | 4S LiPo prototype pack or 4S Li-ion pack with BMS                     | varies   | Better runtime than PP3 9 V, but more safety and charging complexity. See the portable power subsystem above.  |

## Recommended first purchase

For the smallest useful prototype order:

- NVIDIA Jetson Orin Nano Super Developer Kit
- Jetson storage and power supply matched to the exact carrier board
- Jetson active cooling
- Ethernet cable or supported Wi-Fi adapter
- Arduino Nano ESP32
- Short data-capable USB-C cable from Jetson to Nano
- Adafruit ICS-43434 I2S microphone breakout, product 6049
- Adafruit MAX98357A I2S amplifier breakout, product 3006
- Adafruit breadboard-friendly 8 ohm 0.2 W mini speaker, product 1898
- Adafruit NeoPixel Stick 8 x RGBW cool white, product 2869
- Adafruit adjustable breadboard power supply kit, product 184
- DPDT hard-mute switch, or SPST switch plus microphone load-switch circuit
- SPST power switch
- Microphone power-switch/load-switch parts for hard mute
- 74AHCT/74HCT data level shifter for the NeoPixel stick
- 470-1000 uF capacitor
- 330-470 ohm resistor
- Jumper wires and breadboard/perfboard

For **Milestone 5** (vision and tracking), add: Adafruit OV5640 Camera Breakout 72° with Autofocus (product 5945), Adafruit PCA9685 servo driver (product 815), 2× MG90S metal-gear micro servos, 3D-printed three-arm differential neck bracket, M3 ball-link rod ends (×6), M3 threaded rod (\~60 mm per rod), I2C wiring, N20 gearmotor with encoder, DRV8833 breakout, lazy Susan bearing, capsule slip ring.

For the first portable-power add-on:

- 4S 5000-6000 mAh LiPo pack with XT60 connector
- 4S-capable balance charger
- Inline blade fuse holder plus 5 A and 10 A fuses
- DC-rated main power switch
- 19 V buck-boost/boost regulator for the Jetson, at least 3 A continuous
- 4S low-voltage cutoff or protected 4S pack/BMS
- 1S-8S LiPo cell checker/alarm for bench diagnostics
- XT60 pigtails, 5.5 mm x 2.5 mm Jetson barrel pigtail, heat-shrink, and strain relief
- Optional Jetson-powered USB hub or current-limited 5 V accessory rail if LED/audio current exceeds the verified USB budget

## Power budget notes

Treat the Jetson as a separate high-current compute load. Use the
vendor-recommended Jetson supply during bench bring-up. For an enclosed
single-input robot, add a dedicated regulator sized for the exact Jetson carrier
input and power budget.

The Nano ESP32 should be powered from the Jetson over the same USB-C cable used
for the serial link. Do not connect the battery pack or a battery-derived buck
regulator to Nano `VIN` in the wired Jetson build. This keeps the battery
system concerned only with the Jetson input rail and avoids USB/VIN backfeed
questions.

The 5 V LED/audio rail must be treated as a measured USB-powered load. For the
quiet first prototype, keep NeoPixel brightness and speaker volume low and
verify the Jetson USB port, Nano board, and wiring stay within their safe
current limits. If the MAX98357A and NeoPixel stick need more current than the
USB/Nano path can provide, add a Jetson-powered USB hub or current-limited 5 V
accessory rail; do not add a separate battery branch just for the Nano body.

A 4S 5000 mAh pack is about 74 Wh before conversion losses and reserve:

```text
14.8 V nominal * 5 Ah = 74 Wh
```

Budget a real-world usable fraction rather than draining the pack flat. A
Jetson-heavy STT/TTS workload can pull the robot into the 25-35 W range, so a
5000 mAh 4S pack is a roughly 1.5-2.5 hour prototype battery, not an all-day
power source. Portable battery mode requires a hard low-voltage cutoff or a
protected 4S pack/BMS. A balance-plug buzzer is useful while you are nearby on
the bench, but it is not a product safety mechanism.

Before connecting the Jetson to a battery regulator, run these acceptance tests:

| Test              | Pass condition                                                                |
| ----------------- | ----------------------------------------------------------------------------- |
| No-load voltage   | 19.0 V nominal, no startup overshoot above the carrier's allowed input range. |
| Dummy load        | Holds 19 V for at least 30 minutes at 3 A without thermal shutdown.           |
| Load step         | Recovers cleanly when switching between light load and 3 A load.              |
| Low-pack test     | Still regulates when the 4S pack is near cutoff voltage.                      |
| Polarity check    | Center-positive barrel wiring confirmed with a meter at the plug.             |
| Branch protection | Jetson branch has its own fuse or protected distribution path.                |

The Adafruit product 184 kit is convenient and breadboard-friendly, but it is
based on a low-dropout linear regulator rather than a switching buck converter.
It is not part of the preferred runtime power path. Use it for bench testing or
a low-current isolated peripheral experiment, not in parallel with the Jetson
USB-powered Nano/body rail.

A conservative first power target:

| Load                        | Planning current                                                      |
| --------------------------- | --------------------------------------------------------------------- |
| Jetson Orin Nano Super      | 7-25 W depending on power mode and workload                           |
| Nano ESP32 over USB serial  | 150-300 mA bursts                                                     |
| MAX98357A speaker path      | 50-600 mA depending on volume and speaker; keep low on USB power      |
| NeoPixel Stick, 8 RGBW LEDs | 100-500 mA depending on brightness/color; cap brightness aggressively |
| ICS-43434 I2S microphone    | under 1 mA                                                            |

Cap LED brightness and speaker volume in firmware. A rectangular PP3 9 V
battery is no longer part of the default plan.

The ICS-43434 microphone needs 3.3 V. The simplest first build can use the Nano
ESP32 `3V3` pin for that low-current microphone rail after the Nano is powered
from Jetson USB. If you use product 184 or another regulator as a separate
3.3 V rail, use it for peripherals only; do not backfeed the Nano `3V3` pin
unless the board documentation explicitly allows it. Route the microphone's
3.3 V through the hard-mute switch or load switch so the mic is physically
unpowered while muted.

## LED indicator choice

The selected 3bo LED indicator is the Adafruit NeoPixel Stick with 8 x 5050
RGBW LEDs in cool white, product 2869. It provides enough pixels for simple
states like idle breathing, wake flash, listening pulse, thinking sweep, and
speaking meter without the current draw of a larger ring or matrix.

Power the stick from the regulated 5 V rail, tie grounds to the Nano ESP32, and
use a small series resistor on the data line. Product 2869 is RGBW, so firmware
must use a NeoPixel library/configuration that understands four channels per
pixel.

## Microphone choice

The selected 3bo speech microphone is the Adafruit ICS-43434 I2S breakout
because it sends digital audio directly over I2S and fits the ESP32-S3 audio
path well.

The Adafruit MAX9814 electret microphone amplifier, product 1713, is the better
optional analog mic if you want an ADC signal for quick sound-level tests or
sound-reactive LED experiments, because its automatic gain control handles
changing volume better.

The Adafruit MAX4466 electret microphone amplifier, product 1063, is another
optional analog mic with manually adjustable gain. Either analog module can help
compare analog and digital microphone behavior, but neither should replace the
I2S microphone for the main wake-word pipeline unless the firmware is redesigned
around analog sampling.

## Speaker amplifier choice

The selected 3bo speaker amplifier is the Adafruit MAX98357A I2S 3 W class-D
breakout, product 3006. It combines the I2S DAC and mono amplifier stage, so the
Nano ESP32 can stream digital audio to it directly. With a 5 V supply, the
breakout is rated up to 3.2 W into 4 ohm or 1.8 W into 8 ohm at 10% THD.

For the first build, an 8 ohm speaker is the safer default because it draws less
current and gives the breadboard supply more margin. The Adafruit
breadboard-friendly 8 ohm 0.2 W mini speaker, product 1898, is useful for quiet
bring-up because it plugs into a breadboard or perfboard, but it must be kept at
low volume. Consider a temporary 47-100 ohm series resistor for initial tone
tests. A later 8 ohm 1-3 W speaker will sound better for actual spoken
responses. A 4 ohm speaker can be louder, but it makes the 5 V rail current
budget more important.

## Jetson brain choice

The selected local brain is the NVIDIA Jetson Orin Nano Super Developer Kit.
It is overkill for the first LED-and-speaker prototype, in a useful way: it can
run Arbiter locally while also hosting STT/TTS and later vision or richer robot
behaviors.

Recommended first Jetson stack:

- Ubuntu/JetPack on the Jetson.
- `arbiter --api` bound to `127.0.0.1:8080`.
- A 3bo bridge service bound to the LAN, for example `0.0.0.0:8081`, with
  per-device shared-secret authentication and request rate limits.
- `whisper.cpp` or another local STT runtime using small models first.
- Local TTS such as Piper first, cloud TTS as an optional quality upgrade.

Keep model files, provider keys, Arbiter tenant tokens, and conversation logs
on the Jetson, not on the Nano ESP32.

## Reference links

- Arduino Nano ESP32 product/spec page: https://store.arduino.cc/products/nano-esp32
- Arduino Nano ESP32 docs: https://docs.arduino.cc/hardware/nano-esp32/
- Adafruit MAX98357A I2S amplifier: https://www.adafruit.com/product/3006
- Adafruit breadboard-friendly 8 ohm 0.2 W mini speaker: https://www.adafruit.com/product/1898
- Adafruit ICS-43434 I2S microphone breakout: https://www.adafruit.com/product/6049
- Adafruit MAX9814 electret microphone amplifier with AGC: https://www.adafruit.com/product/1713
- Adafruit MAX4466 electret microphone amplifier: https://www.adafruit.com/product/1063
- Adafruit NeoPixel Stick 8 x RGBW cool white: https://www.adafruit.com/product/2869
- Adafruit adjustable breadboard power supply kit: https://www.adafruit.com/product/184
- Pololu step-down voltage regulators: https://www.pololu.com/category/131/step-down-buck-voltage-regulators
- NVIDIA Jetson Orin Nano Super Developer Kit: https://www.nvidia.com/en-us/autonomous-machines/embedded-systems/jetson-orin/nano-super-developer-kit/
- NVIDIA Jetson Orin Nano Developer Kit user guide: https://docs.nvidia.com/jetson/orin-nano-devkit/user-guide/latest/index.html
- whisper.cpp: https://github.com/ggml-org/whisper.cpp
