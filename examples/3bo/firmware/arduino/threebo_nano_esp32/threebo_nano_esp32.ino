#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "threebo_config.h"

// Arduino Nano ESP32 pin labels — used for Arduino API (pinMode / digitalRead /
// digitalWrite).  With BOARD_HAS_PIN_REMAP these are logical numbers that the
// board's remap layer converts to the real ESP32-S3 GPIO.
constexpr int PIN_PIXELS = D6;
constexpr int PIN_MUTE   = D7;
constexpr int PIN_AMP_SD = D8;

// Raw ESP32-S3 GPIO numbers required by the ESP-IDF I2S driver (bypasses the
// Arduino remap layer).  D2=GPIO5, D3=GPIO6, D4=GPIO7, D5=GPIO8.
constexpr int GPIO_I2S_BCLK = 5;   // D2
constexpr int GPIO_I2S_WS   = 6;   // D3
constexpr int GPIO_I2S_MIC  = 7;   // D4  data-in from ICS-43434
constexpr int GPIO_I2S_AMP  = 8;   // D5  data-out to MAX98357A

constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr uint16_t PIXEL_COUNT = 7;
constexpr uint16_t JEWEL_RING_FIRST = 1;
constexpr uint16_t JEWEL_RING_COUNT = 6;
constexpr size_t WAV_HEADER_BYTES = 44;
constexpr size_t AUDIO_CHUNK_BYTES = 512;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t ERROR_HOLD_MS = 1500;

Adafruit_NeoPixel pixels(PIXEL_COUNT, PIN_PIXELS, NEO_GRBW + NEO_KHZ800);

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
bool boot_event_pending = true;
bool prev_muted_state = false;

size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

const char *state_name(RobotState s) {
  switch (s) {
    case RobotState::Boot:          return "boot";
    case RobotState::WifiConnecting:return "wifi_connecting";
    case RobotState::Idle:          return "idle";
    case RobotState::WakeDetected:  return "wake_detected";
    case RobotState::Listening:     return "listening";
    case RobotState::Uploading:     return "uploading";
    case RobotState::Thinking:      return "thinking";
    case RobotState::Speaking:      return "speaking";
    case RobotState::Muted:         return "muted";
    case RobotState::Error:         return "error";
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

bool is_muted() { return digitalRead(PIN_MUTE) == LOW; }

void set_all(uint32_t color) {
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) pixels.setPixelColor(i, color);
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
      const uint16_t active = JEWEL_RING_FIRST + (now / 120) % JEWEL_RING_COUNT;
      pixels.setPixelColor(0, pixels.Color(0, 0, 8, 0));
      pixels.setPixelColor(active, pixels.Color(0, 0, 48, 0));
      pixels.show();
      break;
    }

    case RobotState::Idle: {
      const uint8_t phase = (now / 28) % 80;
      const uint8_t triangle = phase < 40 ? phase : 79 - phase;
      set_all(pixels.Color(0, 0, 0, 2 + triangle / 3));
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
      const uint16_t active = JEWEL_RING_FIRST + (now / 80) % JEWEL_RING_COUNT;
      pixels.setPixelColor(0, pixels.Color(0, 0, 12, 0));
      for (uint16_t i = JEWEL_RING_FIRST; i < JEWEL_RING_FIRST + JEWEL_RING_COUNT; ++i)
        pixels.setPixelColor(i, pixels.Color(0, 0, i == active ? 52 : 5, 0));
      pixels.show();
      break;
    }

    case RobotState::Thinking: {
      pixels.clear();
      const uint16_t active = JEWEL_RING_FIRST + (now / 100) % JEWEL_RING_COUNT;
      pixels.setPixelColor(0, pixels.Color(18, 9, 0, 0));
      for (uint16_t i = JEWEL_RING_FIRST; i < JEWEL_RING_FIRST + JEWEL_RING_COUNT; ++i)
        pixels.setPixelColor(i, i == active ? pixels.Color(45, 24, 0, 0)
                                            : pixels.Color(4, 2, 0, 0));
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

// Runs once at power-on: ring pixels sequence clockwise, centre pops, full
// flash, then off (~650 ms total).  Confirms the Jewel is wired and the
// sketch loaded before WiFi bring-up begins.
void play_startup_animation() {
  pixels.clear();
  pixels.show();
  for (uint16_t i = 0; i < JEWEL_RING_COUNT; ++i) {
    pixels.setPixelColor(JEWEL_RING_FIRST + i, pixels.Color(0, 0, 0, 180));
    pixels.show();
    delay(60);
  }
  pixels.setPixelColor(0, pixels.Color(0, 0, 0, 220));
  pixels.show();
  delay(120);
  set_all(pixels.Color(0, 0, 0, 255));
  delay(100);
  pixels.clear();
  pixels.show();
  delay(50);
}

// C major arpeggio with a G5 bounce before the final resolve — total ~800 ms.
// Square-wave synthesis; gap_ms of silence between notes adds articulation.
void play_startup_chime() {
  if (!begin_audio_tx()) return;

  struct Note { uint16_t freq; uint16_t dur_ms; uint16_t gap_ms; };
  static const Note melody[] = {
    {523,   75, 18},  // C5
    {659,   75, 18},  // E5
    {784,   75, 18},  // G5
    {1047, 130, 18},  // C6  — first hit
    {784,   60, 12},  // G5  — bounce
    {1047, 290,  0},  // C6  — resolve
  };

  static int16_t buf[256];
  const int16_t amp = 4000;

  for (size_t ni = 0; ni < sizeof(melody) / sizeof(melody[0]); ++ni) {
    const uint32_t freq   = melody[ni].freq;
    const uint32_t total  = (uint32_t)SAMPLE_RATE_HZ * melody[ni].dur_ms / 1000;
    const uint32_t period = SAMPLE_RATE_HZ / freq;
    uint32_t done = 0;
    while (done < total) {
      const size_t chunk = min_size(256, total - done);
      for (size_t i = 0; i < chunk; ++i)
        buf[i] = ((done + i) % period < period / 2) ? amp : -amp;
      size_t written = 0;
      i2s_write(I2S_NUM_0, buf, chunk * sizeof(int16_t), &written, portMAX_DELAY);
      done += chunk;
    }
    // Articulation gap — silence between notes.
    if (melody[ni].gap_ms > 0) {
      const uint32_t gap_samples = (uint32_t)SAMPLE_RATE_HZ * melody[ni].gap_ms / 1000;
      uint32_t gap_done = 0;
      while (gap_done < gap_samples) {
        const size_t chunk = min_size(256, gap_samples - gap_done);
        memset(buf, 0, chunk * sizeof(int16_t));
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, chunk * sizeof(int16_t), &written, portMAX_DELAY);
        gap_done += chunk;
      }
    }
  }

  // Flush so the amp doesn't click when SD goes low.
  memset(buf, 0, sizeof(buf));
  size_t written = 0;
  i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
  delay(20);
  i2s_stop();
}

uint8_t *alloc_audio_buffer(size_t bytes) {
  uint8_t *buf = static_cast<uint8_t *>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf) buf = static_cast<uint8_t *>(malloc(bytes));
  return buf;
}

void put_u16_le(uint8_t *p, uint16_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}
void put_u32_le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

void write_wav_header(uint8_t *wav, uint32_t pcm_bytes) {
  memcpy(wav + 0, "RIFF", 4); put_u32_le(wav + 4, 36 + pcm_bytes);
  memcpy(wav + 8, "WAVE", 4); memcpy(wav + 12, "fmt ", 4);
  put_u32_le(wav + 16, 16);   put_u16_le(wav + 20, 1);
  put_u16_le(wav + 22, 1);    put_u32_le(wav + 24, SAMPLE_RATE_HZ);
  put_u32_le(wav + 28, SAMPLE_RATE_HZ * 2);
  put_u16_le(wav + 32, 2);    put_u16_le(wav + 34, 16);
  memcpy(wav + 36, "data", 4); put_u32_le(wav + 40, pcm_bytes);
}

static void i2s_stop() {
  i2s_driver_uninstall(I2S_NUM_0);
}

bool begin_audio_rx() {
  i2s_stop();
  digitalWrite(PIN_AMP_SD, LOW);
  delay(10);

  // ICS-43434 sends 32-bit I2S frames; audio is in the top 18 bits (MSB-first).
  // We read 32-bit and right-shift 16 to produce 16-bit PCM samples.
  const i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE_HZ,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,  // SEL pin → GND
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 128,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0,
  };
  const i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = GPIO_I2S_BCLK,
    .ws_io_num    = GPIO_I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = GPIO_I2S_MIC,
  };

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("I2S RX install failed");
    audio_rx_ready = false;
    return false;
  }
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("I2S RX pin config failed");
    i2s_stop();
    audio_rx_ready = false;
    return false;
  }

  audio_rx_ready = true;
  return true;
}

bool begin_audio_tx() {
  i2s_stop();
  digitalWrite(PIN_AMP_SD, HIGH);
  delay(10);

  const i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE_HZ,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 128,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,   // outputs silence when DMA buffer is empty
    .fixed_mclk           = 0,
  };
  const i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = GPIO_I2S_BCLK,
    .ws_io_num    = GPIO_I2S_WS,
    .data_out_num = GPIO_I2S_AMP,
    .data_in_num  = I2S_PIN_NO_CHANGE,
  };

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("I2S TX install failed");
    return false;
  }
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("I2S TX pin config failed");
    i2s_stop();
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
    Serial.print("ip="); Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Wi-Fi connection failed");
  return false;
}

bool serial_wake_requested() {
  if (!THREEBO_ENABLE_SERIAL_WAKE) return false;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == 'w' || c == 'W') return true;
  }
  return false;
}

bool energy_wake_detected() {
  if (!THREEBO_ENABLE_ENERGY_WAKE || !audio_rx_ready) return false;
  if (millis() - last_energy_wake_ms < 2500) return false;

  // Read 128 × 32-bit frames, shift to 16-bit for energy estimate.
  int32_t raw[128];
  size_t bytes_read = 0;
  i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes_read, 0);
  const size_t n = bytes_read / sizeof(int32_t);
  if (n == 0) return false;

  int64_t sum = 0;
  for (size_t i = 0; i < n; ++i) {
    const int32_t s = raw[i] >> 16;
    sum += s < 0 ? -s : s;
  }

  const int32_t avg = static_cast<int32_t>(sum / static_cast<int64_t>(n));
  if (avg > THREEBO_ENERGY_WAKE_THRESHOLD) {
    last_energy_wake_ms = millis();
    Serial.print("energy_wake avg="); Serial.println(avg);
    return true;
  }
  return false;
}

bool wake_detected() { return serial_wake_requested() || energy_wake_detected(); }

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
  body += type; body += "\",\"data\":"; body += data_json; body += "}";
  http.POST(body);
  http.end();
  Serial.print("event="); Serial.println(type);
}

uint8_t *record_utterance_wav(size_t *out_len) {
  *out_len = 0;
  if (!audio_rx_ready && !begin_audio_rx()) return nullptr;

  const uint32_t seconds = THREEBO_RECORD_SECONDS > 0 ? THREEBO_RECORD_SECONDS : 1;
  const size_t max_pcm_bytes = seconds * SAMPLE_RATE_HZ * sizeof(int16_t);
  uint8_t *wav = alloc_audio_buffer(WAV_HEADER_BYTES + max_pcm_bytes);
  if (!wav) { Serial.println("audio allocation failed"); return nullptr; }

  write_wav_header(wav, 0);

  size_t written = 0;
  int16_t *pcm16 = reinterpret_cast<int16_t *>(wav + WAV_HEADER_BYTES);
  const uint32_t started = millis();

  // Temporary 32-bit read buffer: ICS-43434 sends 32-bit frames.
  // Right-shift 16 to extract the top 16 bits as the 16-bit PCM sample.
  static int32_t tmp32[AUDIO_CHUNK_BYTES / sizeof(int32_t)];

  while (written < max_pcm_bytes && !is_muted()) {
    const size_t remaining = max_pcm_bytes - written;
    const size_t chunk_samples = min_size(
        sizeof(tmp32) / sizeof(int32_t),
        remaining / sizeof(int16_t));

    size_t bytes_read = 0;
    i2s_read(I2S_NUM_0, tmp32, chunk_samples * sizeof(int32_t),
             &bytes_read, pdMS_TO_TICKS(10));

    if (bytes_read > 0) {
      const size_t n = bytes_read / sizeof(int32_t);
      for (size_t i = 0; i < n; ++i)
        pcm16[written / sizeof(int16_t) + i] = static_cast<int16_t>(tmp32[i] >> 16);
      written += n * sizeof(int16_t);
    }

    animate_leds();

    if (millis() - started > seconds * 1000UL + 500UL) break;
  }

  write_wav_header(wav, written);
  *out_len = WAV_HEADER_BYTES + written;
  Serial.print("recorded_wav_bytes="); Serial.println(*out_len);
  return wav;
}

bool read_response_body(HTTPClient &http, uint8_t **out, size_t *out_len) {
  *out = nullptr; *out_len = 0;

  const int length = http.getSize();
  if (length <= 0) { Serial.println("bridge response needs Content-Length"); return false; }
  if (static_cast<size_t>(length) > THREEBO_MAX_RESPONSE_WAV_BYTES) {
    Serial.println("bridge response too large"); return false;
  }

  uint8_t *body = alloc_audio_buffer(static_cast<size_t>(length));
  if (!body) { Serial.println("response allocation failed"); return false; }

  WiFiClient *stream = http.getStreamPtr();
  size_t read_total = 0;
  const uint32_t started = millis();

  while (read_total < static_cast<size_t>(length) &&
         millis() - started < THREEBO_HTTP_TIMEOUT_MS) {
    animate_leds();
    const int available = stream->available();
    if (available > 0) {
      const size_t chunk = min_size(static_cast<size_t>(available),
                                    static_cast<size_t>(length) - read_total);
      read_total += stream->readBytes(
          reinterpret_cast<char *>(body + read_total), chunk);
    } else {
      delay(5);
    }
  }

  if (read_total != static_cast<size_t>(length)) {
    free(body); Serial.println("bridge response read timed out"); return false;
  }

  *out = body; *out_len = read_total;
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
    free(wav);
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
    Serial.print("bridge status="); Serial.println(status);
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
  if (!begin_audio_tx()) { free(response); return false; }

  // Write PCM payload directly; skip the 44-byte WAV header.
  if (response_len > WAV_HEADER_BYTES) {
    const uint8_t *pcm = response + WAV_HEADER_BYTES;
    const size_t   pcm_len = response_len - WAV_HEADER_BYTES;
    size_t written = 0;
    i2s_write(I2S_NUM_0, pcm, pcm_len, &written, portMAX_DELAY);
  }

  i2s_stop();
  free(response);
  return true;
}

void handle_turn() {
  set_state(RobotState::WakeDetected);
  const uint32_t flash_started = millis();
  while (millis() - flash_started < 250) { animate_leds(); delay(10); }

  if (is_muted()) return;

  send_event("audio.wake_detected", "{}");

  set_state(RobotState::Listening);
  size_t wav_len = 0;
  uint8_t *wav = record_utterance_wav(&wav_len);

  i2s_stop();
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
    while (millis() - error_started < ERROR_HOLD_MS) { animate_leds(); delay(20); }
  }

  begin_audio_rx();
  set_state(is_muted() ? RobotState::Muted : RobotState::Idle);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_MUTE, INPUT_PULLUP);
  prev_muted_state = is_muted();
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);

  pixels.begin();
  pixels.setBrightness(THREEBO_LED_BRIGHTNESS);
  pixels.clear();
  pixels.show();

  play_startup_animation();
  play_startup_chime();

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

  if (!connect_wifi()) { delay(25); return; }

  if (boot_event_pending || state == RobotState::WifiConnecting) {
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

  if (cur_muted) { set_state(RobotState::Muted); delay(25); return; }
  if (state == RobotState::Muted) set_state(RobotState::Idle);

  if (state == RobotState::Idle && wake_detected()) handle_turn();

  delay(5);
}
