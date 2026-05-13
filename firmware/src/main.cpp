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
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <ArduinoWebsockets.h>
#include "AudioTools/AudioLibs/I2SCodecStream.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "pins.h"

using namespace websockets;

#define SLEEP_AFTER_MS  10000UL
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
  gfx->fillRect(0, 20, 128, 90, RGB565_BLACK);
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(4, 30);
  gfx->println(top);
  if (bot) {
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(4, 58);
    gfx->println(bot);
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
  showMsg("ZZZ", "hold B to wake", RGB565_BLUE);
  delay(700);
  gfx->fillScreen(RGB565_BLACK);
  digitalWrite(PIN_LCD_BL,  LOW);
  digitalWrite(PIN_SPKR_EN, LOW);
  rtc_gpio_hold_en((gpio_num_t)PIN_PWR_CTL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_B, 0);
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

void doVoiceTurn() {
  // Connect WiFi (uses saved credentials from prior portal)
  showMsg("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    showMsg("WiFi", "failed", RGB565_RED);
    delay(2000);
    return;
  }

  // Connect WebSocket
  showMsg("Connect...");
  WebsocketsClient ws;
  ws.setInsecure(); // allow self-signed / CF edge certs without pinning
  String wsUrl = buildWsUrl();
  if (!ws.connect(wsUrl)) {
    showMsg("WS", "failed", RGB565_RED);
    delay(2000);
    return;
  }

  // Record while Button B held
  showMsg("REC", "release to send", RGB565_RED);
  codecBeginRec();
  size_t rawLen = 0;
  static uint8_t chunk[512];
  while (digitalRead(PIN_BTN_B) == LOW) {
    if (rawLen >= RAW_MAX) break;
    size_t n = i2s.readBytes(chunk, min(sizeof(chunk), RAW_MAX - rawLen));
    if (n > 0) { memcpy(rawBuf + rawLen, chunk, n); rawLen += n; }
  }
  i2s.end();

  if (!rawLen) { ws.close(); return; }

  // Downsample → WAV → send in chunks
  showMsg("SEND", nullptr, RGB565_YELLOW);
  size_t outSamples = downsample(
    (const int16_t *)rawBuf, rawLen / (REC_CH_HW * REC_BITS / 8),
    (int16_t *)(wavBuf + WAV_HDR_SIZE)
  );
  size_t pcmLen = outSamples * (REC_BITS / 8);
  buildWAVHeader(wavBuf, pcmLen);

  size_t total = WAV_HDR_SIZE + pcmLen;
  for (size_t pos = 0; pos < total; ) {
    size_t n = min((size_t)1024, total - pos);
    ws.sendBinary((const char *)(wavBuf + pos), n);
    pos += n;
  }
  ws.sendText("{\"type\":\"done\"}");

  // Receive pipeline results
  MP3DecoderHelix mp3Decoder;
  EncodedAudioOutput *decoded = nullptr;
  bool pipelineDone = false;

  ws.onMessage([&](WebsocketsMessage msg) {
    if (msg.isBinary()) {
      if (decoded) decoded->write((uint8_t *)msg.rawData().c_str(), msg.rawData().length());
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, msg.data()) != DeserializationError::Ok) return;
    const char *type = doc["type"] | "";

    if (strcmp(type, "transcript") == 0) {
      showMsg("You:", doc["text"] | "...");
    } else if (strcmp(type, "response") == 0) {
      showMsg("AI:", doc["text"] | "...");
    } else if (strcmp(type, "audio_start") == 0) {
      codecBeginPlay();
      digitalWrite(PIN_SPKR_EN, HIGH);
      decoded = new EncodedAudioOutput(&i2s, &mp3Decoder);
      decoded->begin();
    } else if (strcmp(type, "audio_end") == 0) {
      if (decoded) { decoded->end(); delete decoded; decoded = nullptr; }
      i2s.end();
      digitalWrite(PIN_SPKR_EN, LOW);
      pipelineDone = true;
    } else if (strcmp(type, "error") == 0) {
      showMsg("ERR", doc["message"] | "?", RGB565_RED);
      if (decoded) { decoded->end(); delete decoded; decoded = nullptr; }
      pipelineDone = true;
    }
  });

  t = millis();
  while (!pipelineDone && ws.available() && millis() - t < 60000) {
    ws.poll();
  }

  if (decoded) { decoded->end(); delete decoded; }
  ws.close();

  showMsg("Done", "sleeping soon...", RGB565_BLUE);
  delay(SLEEP_AFTER_MS);
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

  // Woke from Button B press → do voice turn
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    doVoiceTurn();
    goSleep();
    return;
  }

  // First boot / manual reset: Button A held OR missing credentials → portal
  if (digitalRead(PIN_BTN_A) == LOW || !strlen(workerUrl) || !strlen(apiKey)) {
    openPortal();
  }

  if (!strlen(workerUrl) || !strlen(apiKey)) {
    showMsg("No config", "hold A on boot", RGB565_RED);
    delay(5000);
  }

  goSleep();
}

void loop() {
  // Deep sleep re-enters setup(); loop() runs only if something goes wrong.
  doVoiceTurn();
  goSleep();
}
