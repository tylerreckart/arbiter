# 3bo firmware

This directory contains the first firmware scaffold for 3bo.

## Arduino bench firmware

`arduino/threebo_nano_esp32/threebo_nano_esp32.ino` is the bring-up firmware for
the current prototype hardware. It is intended to prove:

- Wi-Fi connection to the local bridge.
- Mute switch behavior.
- RGBW NeoPixel state animations.
- I2S microphone recording.
- WAV upload to the bridge.
- WAV response playback through the MAX98357A amplifier.

It does not implement the final keyword detector. It includes serial and
optional energy-based development triggers so the complete turn loop can be
tested before the ESP-SR wake provider is ported.

## Expected bridge behavior

The preferred product link is USB CDC serial over the same USB-C cable that
powers the Nano from the Jetson. The current Arduino bench sketch still uses
Wi-Fi/HTTP so the audio loop can be tested quickly before the serial framing
layer is implemented.

The Wi-Fi/HTTP fallback sketch posts a WAV file:

```http
POST /v1/utterance HTTP/1.1
Authorization: Bearer <THREEBO_DEVICE_SECRET>
Content-Type: audio/wav
Content-Length: <bytes>
```

The Jetson-hosted bridge should return a small 16 kHz mono signed 16-bit
`audio/wav` response with `Content-Length`. The firmware stores the response in
memory and plays it through I2S.

The bridge rejects missing or invalid `Authorization: Bearer` headers before
invoking STT, TTS, or Arbiter. The secret is a local device-pairing credential
only; keep Arbiter tenant tokens and provider keys on the Jetson.

## Arduino setup

Install these libraries in the Arduino IDE:

- Arduino ESP32 board support for Arduino Nano ESP32.
- Adafruit NeoPixel.

Create a `threebo_config.h` next to the sketch using
`threebo_config.example.h` as the template. Keep Arbiter credentials and provider
keys out of this file; only the bridge should store those. Generate a random
`THREEBO_DEVICE_SECRET` and configure the same value in the Jetson bridge.

The sketch controls the MAX98357A shutdown pin on `D8`. If the amplifier
shutdown pin is tied high in hardware instead, leave `D8` unconnected or remove
the shutdown writes from the sketch.

## Production firmware

The production firmware moves to ESP-IDF with ESP-SR WakeNet/AFE. Keep
the same bridge contract and LED state names so the higher-level 3bo behavior
does not change when the wake provider is swapped in.
