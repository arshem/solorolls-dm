// AI-Lite firmware
// First boot: connects as AP "AI-Lite-Setup" — captive portal lets you enter
// WiFi credentials, Worker URL, and API key. Press button B anytime to reopen.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "AudioTools/AudioLibs/I2SCodecStream.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "pins.h"

#define SLEEP_MS      30000UL
#define REC_RATE      16000
#define REC_CH        1
#define REC_BITS      16
#define MAX_REC_SEC   10
#define WAV_HDR_SIZE  44
#define MP3_BUF_SIZE  (300 * 1024)

static const size_t PCM_MAX = (size_t)REC_RATE * (REC_BITS / 8) * REC_CH * MAX_REC_SEC;
static const size_t WAV_MAX = WAV_HDR_SIZE + PCM_MAX;

// ── Persisted settings (NVS) ──────────────────────────────────────────────────
static char workerUrl[128] = "";
static char apiKey[64]     = "";

// ── Config from worker ────────────────────────────────────────────────────────
struct DeviceConfig {
  char name[32]     = "AI-Lite";
  char ttsModel[20] = "melotts";
  char ttsLang[8]   = "en";
  char ttsVoice[20] = "luna";
  bool isStreaming   = false;   // true for aura-2-* models
};
static DeviceConfig cfg;

// ── Display ───────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_MOSI);
Arduino_GFX    *gfx = new Arduino_ST7735(bus, PIN_LCD_RST, 3, false, 128, 128, 0, 0);

// ── Audio ─────────────────────────────────────────────────────────────────────
DriverPins    myPins;
AudioBoard    board(AudioDriverES8311, myPins);
I2SCodecStream i2s(board);

// ── PSRAM buffers ─────────────────────────────────────────────────────────────
static uint8_t *wavBuf = nullptr;
static uint8_t *mp3Buf = nullptr;
static size_t   pcmLen = 0;
static size_t   mp3Len = 0;

static unsigned long lastAct;

// ── WiFiManager (global so button B can reopen portal) ────────────────────────
WiFiManager wm;
WiFiManagerParameter *paramUrl;
WiFiManagerParameter *paramKey;

// ── Display helpers ───────────────────────────────────────────────────────────

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

void showReady() {
  showMsg(cfg.name, "hold A: speak");
}

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

// ── WiFi + captive portal ─────────────────────────────────────────────────────

void setupWiFi() {
  paramUrl = new WiFiManagerParameter("workerUrl", "Worker URL", workerUrl, 127);
  paramKey = new WiFiManagerParameter("apiKey",    "API Key",    apiKey,    63);
  wm.addParameter(paramUrl);
  wm.addParameter(paramKey);
  wm.setConfigPortalTimeout(180);

  showMsg("WiFi...");
  if (!wm.autoConnect("AI-Lite-Setup")) {
    showMsg("WiFi", "failed — sleep", RGB565_RED);
    delay(2000);
    esp_deep_sleep_start();
  }

  // Save any updated params from the portal
  const char *u = paramUrl->getValue();
  const char *k = paramKey->getValue();
  bool changed = false;
  if (strlen(u) > 0 && strcmp(u, workerUrl) != 0) { strlcpy(workerUrl, u, sizeof(workerUrl)); changed = true; }
  if (strlen(k) > 0 && strcmp(k, apiKey)    != 0) { strlcpy(apiKey,    k, sizeof(apiKey));    changed = true; }
  if (changed) savePrefs();
}

void reopenPortal() {
  showMsg("Config", "connect to", RGB565_YELLOW);
  gfx->setTextSize(1);
  gfx->setCursor(4, 58);
  gfx->println("AI-Lite-Setup");

  wm.startConfigPortal("AI-Lite-Setup");

  const char *u = paramUrl->getValue();
  const char *k = paramKey->getValue();
  bool changed = false;
  if (strlen(u) > 0 && strcmp(u, workerUrl) != 0) { strlcpy(workerUrl, u, sizeof(workerUrl)); changed = true; }
  if (strlen(k) > 0 && strcmp(k, apiKey)    != 0) { strlcpy(apiKey,    k, sizeof(apiKey));    changed = true; }
  if (changed) savePrefs();
}

// ── Worker config fetch ───────────────────────────────────────────────────────

bool fetchConfig() {
  if (!strlen(workerUrl) || !strlen(apiKey)) return false;

  HTTPClient http;
  http.begin(String(workerUrl) + "/config");
  http.addHeader("Authorization", String("Bearer ") + apiKey);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  JsonDocument doc;
  deserializeJson(doc, http.getStream());
  http.end();

  strlcpy(cfg.name,     doc["name"]     | "AI-Lite", sizeof(cfg.name));
  strlcpy(cfg.ttsModel, doc["ttsModel"] | "melotts", sizeof(cfg.ttsModel));
  strlcpy(cfg.ttsLang,  doc["ttsLang"]  | "en",      sizeof(cfg.ttsLang));
  strlcpy(cfg.ttsVoice, doc["ttsVoice"] | "luna",    sizeof(cfg.ttsVoice));
  cfg.isStreaming = (strncmp(cfg.ttsModel, "aura-2", 6) == 0);
  return true;
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

  memcpy(buf +  0, "RIFF",    4);  memcpy(buf +  4, &chunkSize,  4);
  memcpy(buf +  8, "WAVE",    4);
  memcpy(buf + 12, "fmt ",    4);  memcpy(buf + 16, &fmtSize,    4);
  memcpy(buf + 20, &fmtPCM,   2);  memcpy(buf + 22, &ch,         2);
  memcpy(buf + 24, &rate,     4);  memcpy(buf + 28, &byteRate,   4);
  memcpy(buf + 32, &blockAlign, 2); memcpy(buf + 34, &bits,       2);
  memcpy(buf + 36, "data",    4);  memcpy(buf + 40, &pcmBytes,   4);
}

// ── Codec helpers ─────────────────────────────────────────────────────────────

void codecBeginRec() {
  AudioInfo info(REC_RATE, REC_CH, REC_BITS);
  auto c         = i2s.defaultConfig(RX_MODE);
  c.copyFrom(info);
  c.input_device  = ADC_INPUT_LINE1;
  c.is_master     = true;
  c.mclk_multiple = 256;
  i2s.begin(c);
  i2s.setInputVolume(0.8f);
}

void codecBeginPlay() {
  // aura-2 outputs at 24 kHz; melotts also ~24 kHz.
  // helix setAudioInfo callback will reconfigure if needed.
  AudioInfo info(24000, 2, 16);
  auto c          = i2s.defaultConfig(TX_MODE);
  c.copyFrom(info);
  c.output_device = DAC_OUTPUT_ALL;
  c.is_master     = true;
  c.mclk_multiple = 256;
  i2s.begin(c);
  i2s.setVolume(0.8f);
}

// ── Record ────────────────────────────────────────────────────────────────────

void recordUntilRelease() {
  pcmLen = 0;
  codecBeginRec();
  showMsg("REC", "release to send", RGB565_RED);

  static uint8_t chunk[512];
  while (digitalRead(PIN_BTN_A) == LOW) {
    if (pcmLen >= PCM_MAX) break;
    size_t n = i2s.readBytes(chunk, sizeof(chunk));
    if (n > 0 && pcmLen + n <= PCM_MAX) {
      memcpy(wavBuf + WAV_HDR_SIZE + pcmLen, chunk, n);
      pcmLen += n;
    }
  }
  i2s.end();
}

// ── HTTP helper ───────────────────────────────────────────────────────────────

void beginHTTP(HTTPClient &http, const char *path) {
  http.begin(String(workerUrl) + path);
  http.addHeader("Authorization", String("Bearer ") + apiKey);
  http.setTimeout(20000);
}

// ── Playback: streaming (aura-2) ──────────────────────────────────────────────
// Feeds HTTP chunks directly into helix decoder → I2S as they arrive.

void playStreaming(HTTPClient &http) {
  WiFiClient *stream = http.getStreamPtr();
  codecBeginPlay();
  digitalWrite(PIN_SPKR_EN, HIGH);

  MP3DecoderHelix mp3Decoder;
  EncodedAudioOutput decoded(&i2s, &mp3Decoder);
  decoded.begin();

  static uint8_t buf[512];
  unsigned long t = millis();
  while (http.connected() && millis() - t < 15000) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
      decoded.write(buf, n);
      t = millis();
    }
  }

  decoded.end();
  i2s.end();
  digitalWrite(PIN_SPKR_EN, LOW);
}

// ── Playback: buffered (melotts) ──────────────────────────────────────────────

void playBuffered(HTTPClient &http) {
  WiFiClient *stream = http.getStreamPtr();
  mp3Len = 0;
  int contentLen = http.getSize();

  if (contentLen > 0 && contentLen <= (int)MP3_BUF_SIZE) {
    while ((int)mp3Len < contentLen && http.connected())  {
      int avail = stream->available();
      if (avail > 0) mp3Len += stream->readBytes(mp3Buf + mp3Len, avail);
    }
  } else {
    unsigned long t = millis();
    while (http.connected() && millis() - t < 8000 && mp3Len < MP3_BUF_SIZE) {
      int avail = stream->available();
      if (avail > 0) {
        mp3Len += stream->readBytes(mp3Buf + mp3Len, min(avail, (int)(MP3_BUF_SIZE - mp3Len)));
        t = millis();
      }
    }
  }
  if (!mp3Len) return;

  codecBeginPlay();
  digitalWrite(PIN_SPKR_EN, HIGH);

  MP3DecoderHelix mp3Decoder;
  EncodedAudioOutput decoded(&i2s, &mp3Decoder);
  decoded.begin();

  size_t pos = 0;
  while (pos < mp3Len) {
    size_t n = min((size_t)512, mp3Len - pos);
    decoded.write(mp3Buf + pos, n);
    pos += n;
  }

  decoded.end();
  i2s.end();
  digitalWrite(PIN_SPKR_EN, LOW);
}

// ── Chat pipeline ─────────────────────────────────────────────────────────────

void doChat() {
  recordUntilRelease();
  if (!pcmLen) return;

  showMsg("SEND", nullptr, RGB565_YELLOW);
  buildWAVHeader(wavBuf, pcmLen);

  HTTPClient http;
  beginHTTP(http, "/chat");
  http.addHeader("Content-Type", "audio/wav");

  int code = http.POST(wavBuf, WAV_HDR_SIZE + pcmLen);
  if (code != 200) {
    char msg[20];
    snprintf(msg, sizeof(msg), "HTTP %d", code);
    showMsg("ERROR", msg, RGB565_RED);
    http.end();
    return;
  }

  showMsg("PLAY", nullptr, RGB565_GREEN);
  if (cfg.isStreaming) {
    playStreaming(http);
  } else {
    playBuffered(http);
  }
  http.end();
}

// ── Sleep ─────────────────────────────────────────────────────────────────────

void goSleep() {
  showMsg("ZZZ", "press A to wake", RGB565_BLUE);
  delay(700);
  gfx->fillScreen(RGB565_BLACK);
  digitalWrite(PIN_LCD_BL,  LOW);
  digitalWrite(PIN_SPKR_EN, LOW);
  rtc_gpio_hold_en((gpio_num_t)PIN_PWR_CTL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_A, 0);
  esp_deep_sleep_start();
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  rtc_gpio_hold_dis((gpio_num_t)PIN_PWR_CTL);
  pinMode(PIN_PWR_CTL,  OUTPUT); digitalWrite(PIN_PWR_CTL,  HIGH);
  pinMode(PIN_BTN_A,    INPUT_PULLUP);
  pinMode(PIN_BTN_B,    INPUT_PULLUP);
  pinMode(PIN_SPKR_EN,  OUTPUT); digitalWrite(PIN_SPKR_EN,  LOW);
  pinMode(PIN_LCD_BL,   OUTPUT); digitalWrite(PIN_LCD_BL,   HIGH);

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 4);
  gfx->println("AI-Lite");
  gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);

  wavBuf = (uint8_t *)ps_malloc(WAV_MAX);
  mp3Buf = (uint8_t *)ps_malloc(MP3_BUF_SIZE);
  if (!wavBuf || !mp3Buf) {
    showMsg("PSRAM", "fail", RGB565_RED);
    while (true) delay(1000);
  }

  myPins.addI2C(PinFunction::CODEC, I2C_SCL, I2C_SDA);
  myPins.addI2S(PinFunction::CODEC, I2S_MCLK, I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN);

  loadPrefs();
  setupWiFi();

  if (!strlen(apiKey) || !strlen(workerUrl)) {
    showMsg("No config", "press B: setup", RGB565_RED);
    // Stay awake for user to press B
    while (!strlen(apiKey) || !strlen(workerUrl)) {
      if (digitalRead(PIN_BTN_B) == LOW) reopenPortal();
      delay(100);
    }
  }

  showMsg("Connecting...");
  if (!fetchConfig()) {
    showMsg("Config", "fetch failed", RGB565_YELLOW);
    delay(1500);
  }

  showReady();
  lastAct = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  if (millis() - lastAct >= SLEEP_MS) goSleep();

  // Button B: reopen config portal
  if (digitalRead(PIN_BTN_B) == LOW) {
    delay(50);
    if (digitalRead(PIN_BTN_B) == LOW) {
      reopenPortal();
      fetchConfig();
      showReady();
      lastAct = millis();
    }
  }

  if (digitalRead(PIN_BTN_A) != LOW) return;

  lastAct = millis();
  doChat();
  showReady();
  lastAct = millis();
}
