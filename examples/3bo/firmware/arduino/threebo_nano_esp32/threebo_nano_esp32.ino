#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "threebo_config.h"

constexpr int PIN_I2S_BCLK = D2;
constexpr int PIN_I2S_WS = D3;
constexpr int PIN_I2S_MIC = D4;
constexpr int PIN_I2S_AMP = D5;
constexpr int PIN_PIXELS = D6;
constexpr int PIN_MUTE = D7;
constexpr int PIN_AMP_SD = D8;

constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr uint16_t PIXEL_COUNT = 8;
constexpr size_t WAV_HEADER_BYTES = 44;
constexpr size_t AUDIO_CHUNK_BYTES = 512;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t ERROR_HOLD_MS = 1500;

Adafruit_NeoPixel pixels(PIXEL_COUNT, PIN_PIXELS, NEO_GRBW + NEO_KHZ800);
I2SClass Audio;

enum class RobotState : uint8_t {
  Boot,
  WifiConnecting,
  Idle,
  WakeDetected,
  Listening,
  Uploading,
  Thinking,
  Speaking,
  Muted,
  Error
};

RobotState state = RobotState::Boot;
uint32_t state_started_ms = 0;
uint32_t last_wifi_attempt_ms = 0;
uint32_t last_energy_wake_ms = 0;
bool audio_rx_ready = false;
bool boot_event_pending = true;  // emit device.boot on first WiFi connect
bool prev_muted_state = false;   // edge-detect mute transitions

size_t min_size(size_t a, size_t b) {
  return a < b ? a : b;
}

const char *state_name(RobotState s) {
  switch (s) {
    case RobotState::Boot: return "boot";
    case RobotState::WifiConnecting: return "wifi_connecting";
    case RobotState::Idle: return "idle";
    case RobotState::WakeDetected: return "wake_detected";
    case RobotState::Listening: return "listening";
    case RobotState::Uploading: return "uploading";
    case RobotState::Thinking: return "thinking";
    case RobotState::Speaking: return "speaking";
    case RobotState::Muted: return "muted";
    case RobotState::Error: return "error";
  }
  return "unknown";
}

void set_state(RobotState next) {
  if (state == next) return;
  state = next;
  state_started_ms = millis();
  Serial.print("state=");
  Serial.println(state_name(state));
}

bool is_muted() {
  return digitalRead(PIN_MUTE) == LOW;
}

void set_all(uint32_t color) {
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void animate_leds() {
  const uint32_t now = millis();
  const uint32_t t = now - state_started_ms;

  switch (state) {
    case RobotState::Boot:
      set_all(pixels.Color(0, 0, 0, 24));
      break;

    case RobotState::WifiConnecting: {
      pixels.clear();
      const uint16_t active = (now / 120) % PIXEL_COUNT;
      pixels.setPixelColor(active, pixels.Color(0, 0, 48, 0));
      pixels.show();
      break;
    }

    case RobotState::Idle: {
      const uint8_t phase = (now / 28) % 80;
      const uint8_t triangle = phase < 40 ? phase : 79 - phase;
      const uint8_t white = 2 + triangle / 3;
      set_all(pixels.Color(0, 0, 0, white));
      break;
    }

    case RobotState::WakeDetected:
      set_all(pixels.Color(0, 0, 0, t < 140 ? 80 : 24));
      break;

    case RobotState::Listening: {
      const uint8_t phase = (now / 22) % 90;
      const uint8_t triangle = phase < 45 ? phase : 89 - phase;
      set_all(pixels.Color(0, 0, 20 + triangle, 0));
      break;
    }

    case RobotState::Uploading: {
      pixels.clear();
      const uint16_t active = (now / 80) % PIXEL_COUNT;
      for (uint16_t i = 0; i < PIXEL_COUNT; ++i) {
        const uint8_t level = i == active ? 52 : 5;
        pixels.setPixelColor(i, pixels.Color(0, 0, level, 0));
      }
      pixels.show();
      break;
    }

    case RobotState::Thinking: {
      pixels.clear();
      const uint16_t active = (now / 100) % PIXEL_COUNT;
      for (uint16_t i = 0; i < PIXEL_COUNT; ++i) {
        if (i == active) {
          pixels.setPixelColor(i, pixels.Color(45, 24, 0, 0));
        } else {
          pixels.setPixelColor(i, pixels.Color(4, 2, 0, 0));
        }
      }
      pixels.show();
      break;
    }

    case RobotState::Speaking: {
      const uint8_t phase = (now / 18) % 60;
      const uint8_t triangle = phase < 30 ? phase : 59 - phase;
      set_all(pixels.Color(0, 18 + triangle, 8, 8 + triangle / 2));
      break;
    }

    case RobotState::Muted:
      set_all(pixels.Color(22, 0, 0, 0));
      break;

    case RobotState::Error:
      set_all((now / 180) % 2 == 0 ? pixels.Color(60, 0, 0, 0)
                                   : pixels.Color(0, 0, 0, 0));
      break;
  }
}

uint8_t *alloc_audio_buffer(size_t bytes) {
  uint8_t *buffer =
      static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) {
    buffer = static_cast<uint8_t *>(malloc(bytes));
  }
  return buffer;
}

void put_u16_le(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void put_u32_le(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void write_wav_header(uint8_t *wav, uint32_t pcm_bytes) {
  memcpy(wav + 0, "RIFF", 4);
  put_u32_le(wav + 4, 36 + pcm_bytes);
  memcpy(wav + 8, "WAVE", 4);
  memcpy(wav + 12, "fmt ", 4);
  put_u32_le(wav + 16, 16);
  put_u16_le(wav + 20, 1);
  put_u16_le(wav + 22, 1);
  put_u32_le(wav + 24, SAMPLE_RATE_HZ);
  put_u32_le(wav + 28, SAMPLE_RATE_HZ * 2);
  put_u16_le(wav + 32, 2);
  put_u16_le(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  put_u32_le(wav + 40, pcm_bytes);
}

bool begin_audio_rx() {
  Audio.end();
  digitalWrite(PIN_AMP_SD, LOW);
  delay(10);
  Audio.setPins(PIN_I2S_BCLK, PIN_I2S_WS, -1, PIN_I2S_MIC);

  if (!Audio.begin(I2S_MODE_STD, SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_32BIT,
                   I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S RX init failed");
    audio_rx_ready = false;
    return false;
  }

  if (!Audio.configureRX(SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_32BIT,
                         I2S_SLOT_MODE_MONO, I2S_RX_TRANSFORM_32_TO_16,
                         I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S RX transform failed");
    audio_rx_ready = false;
    return false;
  }

  audio_rx_ready = true;
  return true;
}

bool begin_audio_tx() {
  Audio.end();
  digitalWrite(PIN_AMP_SD, HIGH);
  delay(10);
  Audio.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_AMP, -1);

  if (!Audio.begin(I2S_MODE_STD, SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_MONO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S TX init failed");
    return false;
  }

  return true;
}

bool connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  const uint32_t now = millis();
  if (now - last_wifi_attempt_ms < WIFI_RETRY_INTERVAL_MS) return false;
  last_wifi_attempt_ms = now;

  set_state(RobotState::WifiConnecting);
  WiFi.mode(WIFI_STA);
  WiFi.begin(THREEBO_WIFI_SSID, THREEBO_WIFI_PASSWORD);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 12000) {
    animate_leds();
    delay(25);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ip=");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Wi-Fi connection failed");
  return false;
}

bool serial_wake_requested() {
  if (!THREEBO_ENABLE_SERIAL_WAKE) return false;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == 'w' || c == 'W') {
      return true;
    }
  }
  return false;
}

bool energy_wake_detected() {
  if (!THREEBO_ENABLE_ENERGY_WAKE || !audio_rx_ready) return false;
  if (millis() - last_energy_wake_ms < 2500) return false;

  int16_t samples[128];
  const size_t wanted = sizeof(samples);
  const size_t got = Audio.readBytes(reinterpret_cast<char *>(samples), wanted);
  if (got < wanted) return false;

  int64_t sum = 0;
  const size_t sample_count = got / sizeof(int16_t);
  for (size_t i = 0; i < sample_count; ++i) {
    const int32_t sample = samples[i];
    sum += sample < 0 ? -sample : sample;
  }

  const int32_t avg = static_cast<int32_t>(sum / sample_count);
  if (avg > THREEBO_ENERGY_WAKE_THRESHOLD) {
    last_energy_wake_ms = millis();
    Serial.print("energy_wake avg=");
    Serial.println(avg);
    return true;
  }

  return false;
}

bool wake_detected() {
  return serial_wake_requested() || energy_wake_detected();
}

// POST a JSON event to the bridge POST /v1/event endpoint.
// Fire-and-forget: the bridge relays it to Arbiter POST /v1/events.
// No-op when THREEBO_ENABLE_EVENTS is false or Wi-Fi is not connected.
// data_json must be a valid JSON object literal (e.g. "{}").
void send_event(const char *type, const char *data_json = "{}") {
  if (!THREEBO_ENABLE_EVENTS) return;
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  const String url = String(THREEBO_BRIDGE_BASE_URL) + "/v1/event";

  if (!http.begin(client, url)) return;
  http.setTimeout(THREEBO_EVENT_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + THREEBO_DEVICE_SECRET);

  String body = "{\"type\":\"";
  body += type;
  body += "\",\"data\":";
  body += data_json;
  body += "}";

  http.POST(body);
  http.end();
  Serial.print("event=");
  Serial.println(type);
}

uint8_t *record_utterance_wav(size_t *out_len) {
  *out_len = 0;
  if (!audio_rx_ready && !begin_audio_rx()) return nullptr;

  const uint32_t seconds = THREEBO_RECORD_SECONDS > 0 ? THREEBO_RECORD_SECONDS : 1;
  const size_t max_pcm_bytes = seconds * SAMPLE_RATE_HZ * sizeof(int16_t);
  uint8_t *wav = alloc_audio_buffer(WAV_HEADER_BYTES + max_pcm_bytes);
  if (!wav) {
    Serial.println("audio allocation failed");
    return nullptr;
  }

  write_wav_header(wav, 0);

  size_t written = 0;
  uint8_t *pcm = wav + WAV_HEADER_BYTES;
  const uint32_t started = millis();

  while (written < max_pcm_bytes && !is_muted()) {
    const size_t remaining = max_pcm_bytes - written;
    const size_t chunk = min_size(AUDIO_CHUNK_BYTES, remaining);
    const size_t got = Audio.readBytes(reinterpret_cast<char *>(pcm + written), chunk);

    if (got > 0) {
      written += got;
    } else {
      delay(1);
    }

    animate_leds();

    if (millis() - started > (seconds * 1000UL + 500UL)) {
      break;
    }
  }

  write_wav_header(wav, written);
  *out_len = WAV_HEADER_BYTES + written;
  Serial.print("recorded_wav_bytes=");
  Serial.println(*out_len);
  return wav;
}

bool read_response_body(HTTPClient &http, uint8_t **out, size_t *out_len) {
  *out = nullptr;
  *out_len = 0;

  const int length = http.getSize();
  if (length <= 0) {
    Serial.println("bridge response needs Content-Length");
    return false;
  }
  if (static_cast<size_t>(length) > THREEBO_MAX_RESPONSE_WAV_BYTES) {
    Serial.println("bridge response too large");
    return false;
  }

  uint8_t *body = alloc_audio_buffer(static_cast<size_t>(length));
  if (!body) {
    Serial.println("response allocation failed");
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t read_total = 0;
  const uint32_t started = millis();

  while (read_total < static_cast<size_t>(length) &&
         millis() - started < THREEBO_HTTP_TIMEOUT_MS) {
    const int available = stream->available();
    if (available > 0) {
      const size_t chunk =
          min_size(static_cast<size_t>(available), static_cast<size_t>(length) - read_total);
      const size_t got = stream->readBytes(reinterpret_cast<char *>(body + read_total), chunk);
      read_total += got;
    } else {
      animate_leds();
      delay(5);
    }
  }

  if (read_total != static_cast<size_t>(length)) {
    free(body);
    Serial.println("bridge response read timed out");
    return false;
  }

  *out = body;
  *out_len = read_total;
  return true;
}

bool upload_utterance_and_play_response(uint8_t *wav, size_t wav_len) {
  set_state(RobotState::Uploading);
  animate_leds();

  WiFiClient client;
  HTTPClient http;
  const String url = String(THREEBO_BRIDGE_BASE_URL) + "/v1/utterance";

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  http.setTimeout(THREEBO_HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("Accept", "audio/wav");
  http.addHeader("X-Sample-Rate", String(SAMPLE_RATE_HZ));
  http.addHeader("X-Device-State", "listening");
  http.addHeader("Authorization", String("Bearer ") + THREEBO_DEVICE_SECRET);

  const int status = http.POST(wav, wav_len);
  free(wav);
  wav = nullptr;

  if (status != HTTP_CODE_OK) {
    Serial.print("bridge status=");
    Serial.println(status);
    http.end();
    return false;
  }

  set_state(RobotState::Thinking);
  uint8_t *response = nullptr;
  size_t response_len = 0;
  const bool read_ok = read_response_body(http, &response, &response_len);
  http.end();

  if (!read_ok) return false;

  set_state(RobotState::Speaking);
  if (!begin_audio_tx()) {
    free(response);
    return false;
  }

  Audio.playWAV(response, response_len);
  Audio.end();
  free(response);
  return true;
}

void handle_turn() {
  set_state(RobotState::WakeDetected);
  const uint32_t flash_started = millis();
  while (millis() - flash_started < 250) {
    animate_leds();
    delay(10);
  }

  if (is_muted()) return;

  send_event("audio.wake_detected", "{}");

  set_state(RobotState::Listening);
  size_t wav_len = 0;
  uint8_t *wav = record_utterance_wav(&wav_len);

  Audio.end();
  audio_rx_ready = false;

  if (!wav || wav_len <= WAV_HEADER_BYTES || is_muted()) {
    if (wav) free(wav);
    Serial.println("utterance discarded");
    send_event("audio.utterance_discarded", "{}");
    return;
  }

  const bool ok = upload_utterance_and_play_response(wav, wav_len);
  if (!ok) {
    send_event("device.error", "{\"source\":\"bridge\"}");
    set_state(RobotState::Error);
    const uint32_t error_started = millis();
    while (millis() - error_started < ERROR_HOLD_MS) {
      animate_leds();
      delay(20);
    }
  }

  begin_audio_rx();
  set_state(is_muted() ? RobotState::Muted : RobotState::Idle);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_MUTE, INPUT_PULLUP);
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);

  pixels.begin();
  pixels.setBrightness(THREEBO_LED_BRIGHTNESS);
  pixels.clear();
  pixels.show();

  set_state(RobotState::Boot);
  animate_leds();

  connect_wifi();
  if (!begin_audio_rx()) {
    set_state(RobotState::Error);
    send_event("device.audio_error", "{\"stage\":\"init\"}");
  } else {
    set_state(is_muted() ? RobotState::Muted : RobotState::Idle);
  }

  Serial.println("3bo ready. Send 'w' over Serial for a development wake.");
}

void loop() {
  animate_leds();

  if (!connect_wifi()) {
    delay(25);
    return;
  }

  if (state == RobotState::WifiConnecting) {
    const String ip = WiFi.localIP().toString();
    if (boot_event_pending) {
      boot_event_pending = false;
      send_event("device.boot", ("{\"ip\":\"" + ip + "\"}").c_str());
    } else {
      send_event("device.wifi_reconnect", ("{\"ip\":\"" + ip + "\"}").c_str());
    }
    set_state(RobotState::Idle);
  }

  if (!audio_rx_ready && state == RobotState::Idle && !begin_audio_rx()) {
    set_state(RobotState::Error);
    delay(25);
    return;
  }

  const bool cur_muted = is_muted();
  if (cur_muted != prev_muted_state) {
    prev_muted_state = cur_muted;
    send_event(cur_muted ? "device.mute" : "device.unmute", "{}");
  }

  if (cur_muted) {
    set_state(RobotState::Muted);
    delay(25);
    return;
  }

  if (state == RobotState::Muted) {
    set_state(RobotState::Idle);
  }

  if (state == RobotState::Idle && wake_detected()) {
    handle_turn();
  }

  delay(5);
}
