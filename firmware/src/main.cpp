// AI-Lite firmware
//
// Power-on: Button B held OR no saved creds → config portal → sleep.
// Sleep wake (Button A): start WiFi async, record voice immediately, release A →
//   send WAV → STT/LLM/TTS → play response → idle.
// Idle: A = new turn (reset timeout), B held = config portal, timeout = sleep.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include "AudioTools.h"
#include "AudioTools/AudioLibs/I2SCodecStream.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "pins.h"
#include "portal.h"

#define SLEEP_AFTER_MS  15000UL
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

static char     wifiSsid[64]      = "";
static char     wifiPass[64]      = "";
static char     workerUrl[128]    = "";
static char     apiKey[64]        = "";
static char     assistantName[64] = "AI-Lite";
static uint32_t cachedIp          = 0;
static uint32_t cachedGw          = 0;
static uint32_t cachedSn          = 0;
static uint32_t cachedDns         = 0;

// ── Display ───────────────────────────────────────────────────────────────────

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_MOSI);
Arduino_GFX    *gfx = new Arduino_ST7735(bus, PIN_LCD_RST, 3, false, 128, 128, 0, 0);

void showHeader() {
  gfx->fillRect(0, 0, 128, 14, RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 4);
  gfx->println(assistantName);
  gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);
}

// System messages (WiFi, SEND, errors): big label + optional small subtext.
void showMsg(const char *top, const char *bot = nullptr, uint16_t col = RGB565_WHITE) {
  gfx->fillRect(0, 20, 128, 108, RGB565_BLACK);
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(4, 30);
  gfx->println(top);
  if (bot) {
    char buf[85];
    strncpy(buf, bot, 84);
    buf[84] = '\0';
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(4, 58);
    gfx->println(buf);
  }
}

// Chat text: full-width word-wrap, no label. Yellow = user, white = AI.
void showChat(const char *text, uint16_t col) {
  gfx->fillRect(0, 20, 128, 108, RGB565_BLACK);
  gfx->setTextColor(col);
  gfx->setTextSize(1);
  gfx->setTextWrap(true);
  gfx->setCursor(0, 20);
  gfx->print(text);
}

// ── Audio ─────────────────────────────────────────────────────────────────────

DriverPins     myPins;
AudioBoard     board(AudioDriverES8311, myPins);
I2SCodecStream i2s(board);

static uint8_t *rawBuf = nullptr;
static uint8_t *wavBuf = nullptr;

// ── NVS ───────────────────────────────────────────────────────────────────────

void loadPrefs() {
  Preferences prefs;
  prefs.begin("ailite", true);
  prefs.getString("wifiSsid",      wifiSsid,      sizeof(wifiSsid));
  prefs.getString("wifiPass",      wifiPass,      sizeof(wifiPass));
  prefs.getString("workerUrl",     workerUrl,     sizeof(workerUrl));
  prefs.getString("apiKey",        apiKey,        sizeof(apiKey));
  prefs.getString("assistantName", assistantName, sizeof(assistantName));
  cachedIp  = prefs.getUInt("ip",  0);
  cachedGw  = prefs.getUInt("gw",  0);
  cachedSn  = prefs.getUInt("sn",  0);
  cachedDns = prefs.getUInt("dns", 0);
  prefs.end();
}

void savePrefs() {
  Preferences prefs;
  prefs.begin("ailite", false);
  prefs.putString("wifiSsid",  wifiSsid);
  prefs.putString("wifiPass",  wifiPass);
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
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_A, 0);
  esp_deep_sleep_start();
}

// ── Config portal ─────────────────────────────────────────────────────────────

void openPortal() {
  showMsg("Setup", "AI-Lite-Setup", RGB565_YELLOW);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("AI-Lite-Setup");
  delay(100);

  DNSServer dns;
  dns.start(53, "*", IPAddress(192, 168, 4, 1));

  WebServer server(80);

  server.on("/save", HTTP_POST, [&]() {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      server.send(400, "text/plain", "bad json"); return;
    }
    strlcpy(wifiSsid,  doc["ssid"]      | "", sizeof(wifiSsid));
    strlcpy(wifiPass,  doc["password"]  | "", sizeof(wifiPass));
    strlcpy(workerUrl, doc["workerUrl"] | "", sizeof(workerUrl));
    strlcpy(apiKey,    doc["apiKey"]    | "", sizeof(apiKey));
    savePrefs();
    // Clear IP cache — new network may have different address.
    Preferences p; p.begin("ailite", false);
    p.putUInt("ip", 0); p.putUInt("gw", 0); p.putUInt("sn", 0); p.putUInt("dns", 0);
    p.end();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    delay(500);
    ESP.restart();
  });

  server.onNotFound([&]() {
    int n = WiFi.scanNetworks();
    String opts = "";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      String sel = (ssid == String(wifiSsid)) ? " selected" : "";
      opts += "<option value=\"" + ssid + "\"" + sel + ">" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>\n";
    }
    WiFi.scanDelete();
    String html = String(PORTAL_HTML);
    html.replace("{{WIFI_OPTIONS}}", opts);
    html.replace("{{SSID}}",        String(wifiSsid));
    html.replace("{{PASS}}",        String(wifiPass));
    html.replace("{{WORKER_URL}}",  String(workerUrl));
    html.replace("{{API_KEY}}",     String(apiKey));
    server.send(200, "text/html", html);
  });

  server.begin();

  unsigned long start = millis();
  while (millis() - start < 300000UL) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
}

// ── WAV ───────────────────────────────────────────────────────────────────────

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

// ── Codec ─────────────────────────────────────────────────────────────────────

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

// ── WebSocket ─────────────────────────────────────────────────────────────────
// WS task (Core 0) runs g_ws.loop() under mutex continuously.
// Main task (Core 1) owns codec + display; all shared state serialized by g_mutex.

static SemaphoreHandle_t  g_mutex       = nullptr;
static WebSocketsClient   g_ws;
static volatile bool      g_wsTaskRun   = false;
static volatile bool      g_wsConnected = false;
static volatile bool      g_pipeDone    = false;
static size_t             g_audioBytes  = 0;
static MP3DecoderHelix    *g_mp3        = nullptr;
static EncodedAudioOutput *g_decoded    = nullptr;

static inline void hwLock()   { xSemaphoreTake(g_mutex, portMAX_DELAY); }
static inline void hwUnlock() { xSemaphoreGive(g_mutex); }

static void stopAudio() {
  if (g_decoded) { g_decoded->end(); delete g_decoded; g_decoded = nullptr; }
  if (g_mp3)    { delete g_mp3;    g_mp3    = nullptr; }
  i2s.end();
  digitalWrite(PIN_SPKR_EN, LOW);
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  // Called from wsTask under g_mutex.
  switch (type) {
    case WStype_CONNECTED:
      g_wsConnected = true;
      break;
    case WStype_BIN:
      g_audioBytes += length;
      if (g_decoded) g_decoded->write(payload, length);
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length) != DeserializationError::Ok) break;
      const char *t = doc["type"] | "";
      if (strcmp(t, "transcript") == 0) {
        showChat(doc["text"] | "...", RGB565_YELLOW);
      } else if (strcmp(t, "response") == 0) {
        showChat(doc["text"] | "...", RGB565_WHITE);
      } else if (strcmp(t, "audio_start") == 0) {
        g_audioBytes = 0;
        codecBeginPlay();
        digitalWrite(PIN_SPKR_EN, HIGH);
        g_mp3     = new MP3DecoderHelix();
        g_decoded = new EncodedAudioOutput(&i2s, g_mp3);
        g_decoded->begin();
      } else if (strcmp(t, "audio_end") == 0) {
        g_pipeDone = true;
      } else if (strcmp(t, "error") == 0) {
        showMsg("ERR", doc["message"] | "?", RGB565_RED);
        g_pipeDone = true;
      }
      break;
    }
    default: break;
  }
}

static void wsTask(void *) {
  while (g_wsTaskRun) {
    hwLock();
    g_ws.loop();
    hwUnlock();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  vTaskDelete(nullptr);
}

// ── Name fetch ────────────────────────────────────────────────────────────────

void fetchName() {
  String url = String(workerUrl);
  while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
  url += "/config";
  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + apiKey);
  if (http.GET() == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      const char *n = doc["name"] | "";
      if (strlen(n) > 0 && strcmp(n, assistantName) != 0) {
        strlcpy(assistantName, n, sizeof(assistantName));
        Preferences prefs;
        prefs.begin("ailite", false);
        prefs.putString("assistantName", assistantName);
        prefs.end();
        showHeader();
      }
    }
  }
  http.end();
}

// ── Voice turn ────────────────────────────────────────────────────────────────

void doVoiceTurn() {
  g_wsConnected = false;
  g_pipeDone    = false;
  g_decoded     = nullptr;
  g_mp3         = nullptr;
  g_audioBytes  = 0;

  // Kick off WiFi async so it connects while we record.
  // Cached IP skips DHCP and cuts connect time from ~4s to ~1s.
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (cachedIp) WiFi.config(IPAddress(cachedIp), IPAddress(cachedGw), IPAddress(cachedSn), IPAddress(cachedDns));
    WiFi.begin(wifiSsid, wifiPass);
  }

  // Record immediately — A is already held at entry.
  showMsg("REC", "release A to send", RGB565_RED);
  codecBeginRec();
  static uint8_t chunk[512];
  size_t rawLen = 0;
  while (digitalRead(PIN_BTN_A) == LOW) {
    if (rawLen >= RAW_MAX) break;
    size_t n = i2s.readBytes(chunk, min(sizeof(chunk), RAW_MAX - rawLen));
    if (n > 0) { memcpy(rawBuf + rawLen, chunk, n); rawLen += n; }
  }
  i2s.end();
  if (!rawLen) return;

  // Wait for WiFi (usually already connected by now).
  showMsg("WiFi...");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    showMsg("WiFi", "failed", RGB565_RED); delay(2000); return;
  }

  // Connect WebSocket.
  showMsg("Connect...");
  {
    String wsUrl = String(workerUrl);
    bool isSecure = wsUrl.startsWith("https://");
    if      (isSecure)               wsUrl = "wss://" + wsUrl.substring(8);
    else if (wsUrl.startsWith("http://")) wsUrl = "ws://"  + wsUrl.substring(7);
    while (wsUrl.endsWith("/")) wsUrl = wsUrl.substring(0, wsUrl.length() - 1);
    wsUrl += "/ws?key=";
    wsUrl += apiKey;

    String noScheme = wsUrl.substring(isSecure ? 6 : 5);
    int slash = noScheme.indexOf('/');
    String wsHost = noScheme.substring(0, slash);
    String wsPath = noScheme.substring(slash);

    g_ws.onEvent(onWsEvent);
    if (isSecure) { g_ws.beginSSL(wsHost, 443, wsPath); }
    else            g_ws.begin(wsHost, 80, wsPath);
    g_ws.setReconnectInterval(0);
  }
  g_wsTaskRun = true;
  xTaskCreatePinnedToCore(wsTask, "ws", 8192, nullptr, 2, nullptr, 0);
  t = millis();
  while (!g_wsConnected && millis() - t < 10000) delay(20);
  if (!g_wsConnected) {
    g_wsTaskRun = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    showMsg("WS", "failed", RGB565_RED); delay(2000); return;
  }

  // Downsample → WAV → send.
  size_t outSamples = downsample(
    (const int16_t *)rawBuf, rawLen / (REC_CH_HW * REC_BITS / 8),
    (int16_t *)(wavBuf + WAV_HDR_SIZE)
  );
  size_t pcmLen = outSamples * (REC_BITS / 8);
  buildWAVHeader(wavBuf, pcmLen);
  size_t total = WAV_HDR_SIZE + pcmLen;

  hwLock();
  showMsg("SEND", nullptr, RGB565_YELLOW);
  for (size_t pos = 0; pos < total; ) {
    size_t n = min((size_t)1024, total - pos);
    g_ws.sendBIN(wavBuf + pos, n);
    pos += n;
  }
  g_ws.sendTXT("{\"type\":\"done\"}");
  hwUnlock();

  // Wait for pipeline response; A press interrupts playback.
  bool interrupted = false;
  t = millis();
  while (!g_pipeDone && millis() - t < 60000) {
    if (digitalRead(PIN_BTN_A) == LOW) { interrupted = true; break; }
    delay(10);
  }
  if (!interrupted && g_pipeDone) {
    // Drain remaining MP3 DMA buffer. Aura-2 ≈ 32 kbps → 4000 bytes/s.
    unsigned long playMs = (g_audioBytes * 1000UL / 4000UL) + 1000UL;
    t = millis();
    while (millis() - t < playMs) {
      if (digitalRead(PIN_BTN_A) == LOW) break;
      delay(10);
    }
  }

  hwLock(); stopAudio(); hwUnlock();
  g_wsTaskRun = false;
  vTaskDelay(pdMS_TO_TICKS(50));
  g_ws.disconnect();
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

  rawBuf = (uint8_t *)ps_malloc(RAW_MAX);
  wavBuf = (uint8_t *)ps_malloc(WAV_MAX);
  if (!rawBuf || !wavBuf) { showMsg("PSRAM", "fail", RGB565_RED); while (true) delay(1000); }

  myPins.addI2C(PinFunction::CODEC, I2C_SCL, I2C_SDA);
  myPins.addI2S(PinFunction::CODEC, I2S_MCLK, I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN);

  g_mutex = xSemaphoreCreateMutex();
  loadPrefs();
  showHeader(); // after loadPrefs so cached assistantName is shown

  // B held at boot → config portal.
  delay(50);
  if (digitalRead(PIN_BTN_B) == LOW) {
    openPortal();
    goSleep();
    return;
  }

  // Missing creds → config portal.
  if (!strlen(wifiSsid) || !strlen(workerUrl) || !strlen(apiKey)) {
    showMsg("No config", "hold B: setup", RGB565_RED);
    delay(2000);
    openPortal();
    goSleep();
    return;
  }

  bool fromSleep = (esp_reset_reason() == ESP_RST_DEEPSLEEP);

  if (!fromSleep) {
    // Power-cycle: connect WiFi (full DHCP), refresh name and IP cache.
    // Only writes NVS if values actually changed — protects flash lifetime.
    showMsg("WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifiSsid, wifiPass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      uint32_t newIp  = (uint32_t)WiFi.localIP();
      uint32_t newGw  = (uint32_t)WiFi.gatewayIP();
      uint32_t newSn  = (uint32_t)WiFi.subnetMask();
      uint32_t newDns = (uint32_t)WiFi.dnsIP();
      if (newIp != cachedIp || newGw != cachedGw || newSn != cachedSn || newDns != cachedDns) {
        cachedIp = newIp; cachedGw = newGw; cachedSn = newSn; cachedDns = newDns;
        Preferences prefs;
        prefs.begin("ailite", false);
        prefs.putUInt("ip", cachedIp); prefs.putUInt("gw", cachedGw);
        prefs.putUInt("sn", cachedSn); prefs.putUInt("dns", cachedDns);
        prefs.end();
      }
      fetchName(); // writes NVS only if name changed
    }
    showHeader();
  }

  // Woke from deep sleep via Button A → voice turn immediately.
  if (fromSleep) {
    doVoiceTurn();
  }

  // Idle loop: A = new voice turn, B = config, timeout = sleep.
  showMsg("Ready", "hold A: speak", RGB565_GREEN);
  unsigned long idleStart = millis();
  while (true) {
    if (digitalRead(PIN_BTN_B) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_B) == LOW) { openPortal(); break; }
    }
    if (digitalRead(PIN_BTN_A) == LOW) {
      doVoiceTurn();
      showMsg("Ready", "hold A: speak", RGB565_GREEN);
      idleStart = millis();
    }
    if (millis() - idleStart > SLEEP_AFTER_MS) break;
    delay(20);
  }

  goSleep();
}

void loop() {}
