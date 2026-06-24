#pragma once

// Local network settings for the Wi-Fi/HTTP fallback bench sketch.
// The preferred product link is USB CDC serial over the Jetson USB cable.
constexpr char THREEBO_WIFI_SSID[] = "your-wifi-ssid";
constexpr char THREEBO_WIFI_PASSWORD[] = "your-wifi-password";

// Jetson bridge URL.  The firmware posts to THREEBO_BRIDGE_BASE_URL/v1/utterance.
// Use the Jetson's mDNS name (requires avahi-daemon on the Jetson) or its LAN IP.
// Example with mDNS:   "http://3bo.local:8081"
// Example with IP:     "http://192.168.1.42:8081"
constexpr char THREEBO_BRIDGE_BASE_URL[] = "http://3bo.local:8081";

// Per-device shared secret.  Must match THREEBO_DEVICE_SECRET on the Jetson.
// Sent as: Authorization: Bearer <THREEBO_DEVICE_SECRET>
// Generate a random value, e.g.:  openssl rand -hex 32
constexpr char THREEBO_DEVICE_SECRET[] = "replace-with-random-device-secret";

// Development wake triggers. These are not the final keyword detector.
constexpr bool THREEBO_ENABLE_SERIAL_WAKE = true;  // Send 'w' over Serial.
constexpr bool THREEBO_ENABLE_ENERGY_WAKE = false;
constexpr int32_t THREEBO_ENERGY_WAKE_THRESHOLD = 1200;

// Keep first tests gentle for the 0.2 W speaker and 8-pixel LED stick.
constexpr uint8_t  THREEBO_LED_BRIGHTNESS        = 28;
constexpr uint8_t  THREEBO_RECORD_SECONDS        = 4;
constexpr size_t   THREEBO_MAX_RESPONSE_WAV_BYTES = 512 * 1024;
constexpr uint32_t THREEBO_HTTP_TIMEOUT_MS       = 30000;

// Hardware event reporting.  When enabled, the firmware POSTs state transitions
// (mute, error, Wi-Fi reconnect) to the Jetson bridge as JSON events, which
// the bridge forwards to Arbiter POST /v1/events for agent-driven handling.
// Events are fire-and-forget; keep the timeout short since the bridge is LAN-local.
constexpr bool     THREEBO_ENABLE_EVENTS    = true;
constexpr uint32_t THREEBO_EVENT_TIMEOUT_MS = 1000;
