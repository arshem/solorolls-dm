// AI-Lite firmware — WebSocket voice assistant
//
// Boot (Button A held OR no credentials): open WiFi captive portal to configure
//   Worker URL and API key; then sleep.
// Boot (normal): sleep immediately; wait for Button B.
//
// Wake (Button B held): connect WiFi → WebSocket → record audio while B held →
//   release sends to worker (STT → LLM → TTS) → play streaming MP3 back →
//   wait 10 s → sleep.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <WebSocketsClient.h>
#include "AudioTools.h"
#include "AudioTools/AudioLibs/I2SCodecStream.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "pins.h"

#define SLEEP_AFTER_MS  30000UL
#define REC_RATE_HW     24000
#define REC_CH_HW       2
#define REC_BITS        16
#define REC_RATE        16000
#define REC_CH          1
#define MAX_REC_SEC     10
#define WAV_HDR_SIZE    44

static const size_t RAW_MAX = (size_t)REC_RATE_HW * (REC_CH_HW * REC_BITS / 8) * MAX_REC_SEC;
static const size_t PCM_MAX = (size_t)REC_RATE    * (REC_CH    * REC_BITS / 8) * MAX_REC_SEC;
static const size_t WAV_MAX = WAV_HDR_SIZE + PCM_MAX;

static char workerUrl[128] = "";
static char apiKey[64]     = "";

// ── Display ───────────────────────────────────────────────────────────────────

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_MOSI);
Arduino_GFX    *gfx = new Arduino_ST7735(bus, PIN_LCD_RST, 3, false, 128, 128, 0, 0);

void showMsg(const char *top, const char *bot = nullptr, uint16_t col = RGB565_WHITE) {
  gfx->fillRect(0, 20, 128, 108, RGB565_BLACK);
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(4, 30);
  gfx->println(top);
  if (bot) {
    // truncate to ~84 chars (7 lines × 12 chars at textSize 1)
    char buf[85];
    strncpy(buf, bot, 84);
    buf[84] = '\0';
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(4, 58);
    gfx->println(buf);
  }
}

// ── Audio ─────────────────────────────────────────────────────────────────────

DriverPins     myPins;
AudioBoard     board(AudioDriverES8311, myPins);
I2SCodecStream i2s(board);

// ── PSRAM ─────────────────────────────────────────────────────────────────────

static uint8_t *rawBuf = nullptr;
static uint8_t *wavBuf = nullptr;

// ── NVS ───────────────────────────────────────────────────────────────────────

void loadPrefs() {
  Preferences prefs;
  prefs.begin("ailite", true);
  prefs.getString("workerUrl", workerUrl, sizeof(workerUrl));
  prefs.getString("apiKey",    apiKey,    sizeof(apiKey));
  prefs.end();
}

void savePrefs() {
  Preferences prefs;
  prefs.begin("ailite", false);
  prefs.putString("workerUrl", workerUrl);
  prefs.putString("apiKey",    apiKey);
  prefs.end();
}

// ── Sleep ─────────────────────────────────────────────────────────────────────

void goSleep() {
  showMsg("ZZZ", "hold A to wake", RGB565_BLUE);
  delay(700);
  gfx->fillScreen(RGB565_BLACK);
  digitalWrite(PIN_LCD_BL,  LOW);
  digitalWrite(PIN_SPKR_EN, LOW);
  rtc_gpio_hold_en((gpio_num_t)PIN_PWR_CTL);
  // PIN_BTN_A = GPIO 1 is RTC-capable; PIN_BTN_B = GPIO 42 is not
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_A, 0);
  esp_deep_sleep_start();
}

// ── Captive portal ────────────────────────────────────────────────────────────

void openPortal() {
  WiFiManager wm;
  WiFiManagerParameter paramUrl("workerUrl", "Worker URL", workerUrl, 127);
  WiFiManagerParameter paramKey("apiKey",    "API Key",    apiKey,    63);
  wm.addParameter(&paramUrl);
  wm.addParameter(&paramKey);
  wm.setConfigPortalTimeout(300);

  showMsg("Setup", "AI-Lite-Setup", RGB565_YELLOW);
  wm.startConfigPortal("AI-Lite-Setup");

  bool changed = false;
  const char *u = paramUrl.getValue();
  const char *k = paramKey.getValue();
  if (strlen(u) > 0 && strcmp(u, workerUrl) != 0) { strlcpy(workerUrl, u, sizeof(workerUrl)); changed = true; }
  if (strlen(k) > 0 && strcmp(k, apiKey)    != 0) { strlcpy(apiKey,    k, sizeof(apiKey));    changed = true; }
  if (changed) savePrefs();
}

// ── WAV header ────────────────────────────────────────────────────────────────

void buildWAVHeader(uint8_t *buf, uint32_t pcmBytes) {
  const uint32_t rate       = REC_RATE;
  const uint16_t ch         = REC_CH;
  const uint16_t bits       = REC_BITS;
  const uint32_t byteRate   = rate * ch * (bits / 8);
  const uint16_t blockAlign = ch * (bits / 8);
  const uint32_t chunkSize  = 36 + pcmBytes;
  const uint16_t fmtPCM     = 1;
  const uint32_t fmtSize    = 16;

  memcpy(buf +  0, "RIFF",      4); memcpy(buf +  4, &chunkSize,   4);
  memcpy(buf +  8, "WAVE",      4);
  memcpy(buf + 12, "fmt ",      4); memcpy(buf + 16, &fmtSize,     4);
  memcpy(buf + 20, &fmtPCM,     2); memcpy(buf + 22, &ch,          2);
  memcpy(buf + 24, &rate,       4); memcpy(buf + 28, &byteRate,    4);
  memcpy(buf + 32, &blockAlign, 2); memcpy(buf + 34, &bits,        2);
  memcpy(buf + 36, "data",      4); memcpy(buf + 40, &pcmBytes,    4);
}

// Downsample 24 kHz stereo int16 → 16 kHz mono int16 (3:2 ratio, linear interp).
size_t downsample(const int16_t *src, size_t srcFrames, int16_t *dst) {
  size_t out = 0, i = 0;
  while (i + 2 < srcFrames) {
    int32_t m0 = ((int32_t)src[i*2]     + src[i*2+1])     / 2;
    int32_t m1 = ((int32_t)src[(i+1)*2] + src[(i+1)*2+1]) / 2;
    int32_t m2 = ((int32_t)src[(i+2)*2] + src[(i+2)*2+1]) / 2;
    dst[out++] = (int16_t)m0;
    dst[out++] = (int16_t)((m1 + m2) / 2);
    i += 3;
  }
  return out;
}

// ── Codec helpers ─────────────────────────────────────────────────────────────

void codecBeginRec() {
  AudioInfo info(REC_RATE_HW, REC_CH_HW, REC_BITS);
  auto c         = i2s.defaultConfig(RX_MODE);
  c.copyFrom(info);
  c.input_device  = ADC_INPUT_LINE1;
  c.is_master     = true;
  c.mclk_multiple = 256;
  i2s.begin(c);
  i2s.setInputVolume(0.8f);
}

void codecBeginPlay() {
  AudioInfo info(24000, 2, 16);
  auto c          = i2s.defaultConfig(TX_MODE);
  c.copyFrom(info);
  c.output_device = DAC_OUTPUT_ALL;
  c.is_master     = true;
  c.mclk_multiple = 256;
  i2s.begin(c);
  i2s.setVolume(0.8f);
}

// ── Build WebSocket URL ───────────────────────────────────────────────────────

String buildWsUrl() {
  String u = String(workerUrl);
  if      (u.startsWith("https://")) u = "wss://" + u.substring(8);
  else if (u.startsWith("http://"))  u = "ws://"  + u.substring(7);
  while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
  return u + "/ws?key=" + String(apiKey);
}

// ── Voice turn ────────────────────────────────────────────────────────────────
// Core 0: WS task runs ws.loop() under mutex continuously.
// Core 1: main thread checks button, drives state; never calls ws.loop() itself.
// All codec + display + WS operations are protected by g_mutex.

static SemaphoreHandle_t  g_mutex        = nullptr;
static WebSocketsClient   g_ws;
static volatile bool      g_wsTaskRun    = false;
static TaskHandle_t       g_wsTaskHandle = nullptr;

// Shared state (written under mutex or from atomic flag ops)
static MP3DecoderHelix    *g_mp3          = nullptr;
static EncodedAudioOutput *g_decoded      = nullptr;
static volatile bool       g_wsConnected  = false;
static volatile bool       g_pipelineDone = false;
static volatile bool       g_recording    = false;
static size_t              g_audioBytes   = 0;

static inline void hwLock()   { xSemaphoreTake(g_mutex, portMAX_DELAY); }
static inline void hwUnlock() { xSemaphoreGive(g_mutex); }

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  // Runs on Core 0 inside ws.loop(), already under g_mutex.
  switch (type) {
    case WStype_CONNECTED:
      g_wsConnected = true;
      Serial.println("WS connected");
      break;

    case WStype_DISCONNECTED:
      Serial.println("WS disconnected");
      break;

    case WStype_BIN:
      if (!g_recording) {
        g_audioBytes += length;
        if (g_decoded) g_decoded->write(payload, length);
      }
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length) != DeserializationError::Ok) break;
      const char *t = doc["type"] | "";
      Serial.printf("msg: %s\n", t);

      if (strcmp(t, "transcript") == 0) {
        if (!g_recording) showMsg("You:", doc["text"] | "...");
      } else if (strcmp(t, "response") == 0) {
        if (!g_recording) showMsg("AI:", doc["text"] | "...");
      } else if (strcmp(t, "audio_start") == 0) {
        if (!g_recording) {
          g_audioBytes = 0;
          codecBeginPlay();
          digitalWrite(PIN_SPKR_EN, HIGH);
          g_mp3     = new MP3DecoderHelix();
          g_decoded = new EncodedAudioOutput(&i2s, g_mp3);
          g_decoded->begin();
        }
      } else if (strcmp(t, "audio_end") == 0) {
        g_pipelineDone = true;
      } else if (strcmp(t, "error") == 0) {
        if (!g_recording) showMsg("ERR", doc["message"] | "?", RGB565_RED);
        g_pipelineDone = true;
      }
      break;
    }
    default: break;
  }
}

static void wsLoopTask(void *) {
  while (g_wsTaskRun) {
    hwLock();
    g_ws.loop();
    hwUnlock();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  vTaskDelete(nullptr);
}

static void stopAudio() {
  // Must be called under g_mutex.
  if (g_decoded) { g_decoded->end(); delete g_decoded; g_decoded = nullptr; }
  if (g_mp3)    { delete g_mp3;    g_mp3    = nullptr; }
  i2s.end();
  digitalWrite(PIN_SPKR_EN, LOW);
}

bool doVoiceTurn() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();

  g_wsConnected  = false;
  g_pipelineDone = false;
  g_recording    = false;
  g_decoded      = nullptr;
  g_mp3          = nullptr;

  // ── WiFi ─────────────────────────────────────────────────────────────────────
  showMsg("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin();
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    showMsg("WiFi", "failed", RGB565_RED);
    delay(2000);
    return false;
  }
  delay(500);

  // ── WebSocket ─────────────────────────────────────────────────────────────────
  String wsUrl = buildWsUrl();
  bool isSecure = wsUrl.startsWith("wss://");
  String noScheme = wsUrl.substring(isSecure ? 6 : 5);
  int slash = noScheme.indexOf('/');
  String wsHost = noScheme.substring(0, slash);
  String wsPath = noScheme.substring(slash);
  int wsPort = isSecure ? 443 : 80;
  Serial.printf("WS %s:%d%s\n", wsHost.c_str(), wsPort, wsPath.c_str());

  showMsg("Connect...");
  g_ws.onEvent(onWsEvent);
  if (isSecure) g_ws.beginSSL(wsHost, wsPort, wsPath);
  else          g_ws.begin(wsHost, wsPort, wsPath);
  g_ws.setReconnectInterval(0);

  // Spin up WS task on Core 0
  g_wsTaskRun = true;
  xTaskCreatePinnedToCore(wsLoopTask, "ws", 8192, nullptr, 2, &g_wsTaskHandle, 0);

  // Wait for connection (main thread just polls flag)
  t = millis();
  while (!g_wsConnected && millis() - t < 10000) delay(20);
  if (!g_wsConnected) {
    g_wsTaskRun = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    showMsg("WS", "failed", RGB565_RED);
    delay(5000);
    return false;
  }

  // ── Session loop ──────────────────────────────────────────────────────────────
  static uint8_t chunk[512];
  unsigned long idleStart = millis();

  while (true) {
    // Idle: button check only — WS task keeps connection alive on Core 0
    hwLock(); showMsg("Ready", "hold A: speak", RGB565_GREEN); hwUnlock();
    while (digitalRead(PIN_BTN_A) != LOW) {
      if (millis() - idleStart > SLEEP_AFTER_MS) {
        hwLock(); stopAudio(); g_ws.disconnect(); hwUnlock();
        g_wsTaskRun = false;
        vTaskDelay(pdMS_TO_TICKS(50));
        return false;
      }
      delay(10);
    }

    // A pressed — stop playback immediately (no ws.loop() needed here)
    hwLock(); stopAudio(); hwUnlock();
    g_pipelineDone = false;

    // ── Record ───────────────────────────────────────────────────────────────
    hwLock(); showMsg("REC", "release to send", RGB565_RED); hwUnlock();
    g_recording = true;
    hwLock(); codecBeginRec(); hwUnlock();
    size_t rawLen = 0;
    while (digitalRead(PIN_BTN_A) == LOW) {
      if (rawLen >= RAW_MAX) break;
      // i2s DMA read — no mutex needed (Core 0 is blocked by g_recording flag)
      size_t n = i2s.readBytes(chunk, min(sizeof(chunk), RAW_MAX - rawLen));
      if (n > 0) { memcpy(rawBuf + rawLen, chunk, n); rawLen += n; }
    }
    hwLock(); i2s.end(); hwUnlock();
    g_recording = false;
    if (!rawLen) continue;

    // ── Downsample → WAV → send ───────────────────────────────────────────────
    hwLock(); showMsg("SEND", nullptr, RGB565_YELLOW); hwUnlock();
    size_t outSamples = downsample(
      (const int16_t *)rawBuf, rawLen / (REC_CH_HW * REC_BITS / 8),
      (int16_t *)(wavBuf + WAV_HDR_SIZE)
    );
    size_t pcmLen = outSamples * (REC_BITS / 8);
    buildWAVHeader(wavBuf, pcmLen);

    size_t total = WAV_HDR_SIZE + pcmLen;
    hwLock();
    for (size_t pos = 0; pos < total; ) {
      size_t n = min((size_t)1024, total - pos);
      g_ws.sendBIN(wavBuf + pos, n);
      pos += n;
    }
    g_ws.sendTXT("{\"type\":\"done\"}");
    hwUnlock();

    // ── Wait for response — button always responsive ───────────────────────────
    t = millis();
    while (!g_pipelineDone && millis() - t < 60000) {
      if (digitalRead(PIN_BTN_A) == LOW) goto next_turn; // interrupt playback
      delay(10);
    }

    if (g_pipelineDone) {
      // Audio plays in real-time; only DMA tail remains after audio_end.
      // Aura-2 MP3 ≈ 32 kbps. Add 1 s safety margin. Button still interrupts.
      unsigned long playMs = (g_audioBytes * 1000UL / 4000UL) + 1000UL;
      Serial.printf("drain: %u bytes ~%lu ms\n", g_audioBytes, playMs);
      t = millis();
      while (millis() - t < playMs) {
        if (digitalRead(PIN_BTN_A) == LOW) goto next_turn;
        delay(10);
      }
    }
    hwLock(); stopAudio(); hwUnlock();
    idleStart = millis();
    continue;

    next_turn:
    hwLock(); stopAudio(); hwUnlock();
    g_pipelineDone = false;
    idleStart = millis();
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  rtc_gpio_hold_dis((gpio_num_t)PIN_PWR_CTL);
  pinMode(PIN_PWR_CTL,  OUTPUT); digitalWrite(PIN_PWR_CTL,  HIGH);
  pinMode(PIN_BTN_A,    INPUT_PULLUP);
  pinMode(PIN_BTN_B,    INPUT_PULLUP);
  pinMode(PIN_SPKR_EN,  OUTPUT); digitalWrite(PIN_SPKR_EN,  LOW);
  pinMode(PIN_LCD_BL,   OUTPUT); digitalWrite(PIN_LCD_BL,   HIGH);

  Serial.begin(115200);

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 4);
  gfx->println("AI-Lite");
  gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);

  rawBuf = (uint8_t *)ps_malloc(RAW_MAX);
  wavBuf = (uint8_t *)ps_malloc(WAV_MAX);
  if (!rawBuf || !wavBuf) {
    showMsg("PSRAM", "fail", RGB565_RED);
    while (true) delay(1000);
  }

  myPins.addI2C(PinFunction::CODEC, I2C_SCL, I2C_SDA);
  myPins.addI2S(PinFunction::CODEC, I2S_MCLK, I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN);

  loadPrefs();

  // Woke from Button A (ext0) → check Button B for portal, else voice turn
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    if (digitalRead(PIN_BTN_B) == LOW) {
      openPortal();
    } else {
      doVoiceTurn();
    }
    goSleep();
    return;
  }

  // First boot / manual reset: missing credentials OR Button B held → portal
  if (!strlen(workerUrl) || !strlen(apiKey) || digitalRead(PIN_BTN_B) == LOW) {
    openPortal();
  }

  if (!strlen(workerUrl) || !strlen(apiKey)) {
    showMsg("No config", "hold B on boot", RGB565_RED);
    delay(5000);
  }

  goSleep();
}

void loop() {
  doVoiceTurn();
  goSleep();
}
