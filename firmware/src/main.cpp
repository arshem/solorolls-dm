// SoloRolls — Solo D&D Campaign Device (Gemini Live API)
//
// Real-time bidirectional audio streaming via Gemini Live.
// ESP32 streams 16kHz mono PCM to server, receives 24kHz mono PCM back.
// No separate STT/LLM/TTS — Gemini handles everything natively.
//
// Button A: start/stop voice turn
// Button B (held at boot): config portal
// Idle timeout: deep sleep

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
#include "pins.h"
#include "portal.h"

#define SLEEP_AFTER_MS  120000UL // 2 min idle before sleep (default, adjustable)
#define REC_RATE_HW     24000
#define REC_CH_HW       2
#define REC_BITS        16
#define REC_RATE        16000    // what we send to Gemini
#define REC_CH          1
#define PLAY_RATE       24000   // what Gemini sends back
#define PLAY_CH         1

// ── Multi-WiFi storage ────────────────────────────────────────────────────────
#define MAX_WIFI_NETS   4

struct WifiEntry {
  char ssid[64];
  char pass[64];
};

static WifiEntry  g_wifiNets[MAX_WIFI_NETS];
static int        g_wifiCount    = 0;
static int        g_wifiActive   = -1;  // index of currently connected network

static char     workerUrl[128]    = "";
static char     apiKey[64]        = "";
static char     assistantName[64] = "SoloRolls DM";
static uint32_t cachedIp          = 0;
static uint32_t cachedGw          = 0;
static uint32_t cachedSn          = 0;
static uint32_t cachedDns         = 0;

// ── Settings (persisted to NVS) ───────────────────────────────────────────────
static const float VOL_LEVELS[] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
static const int   VOL_COUNT    = sizeof(VOL_LEVELS) / sizeof(VOL_LEVELS[0]);
static int         g_volIndex   = 3;  // default 80%

static const float MIC_LEVELS[] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
static const int   MIC_COUNT    = sizeof(MIC_LEVELS) / sizeof(MIC_LEVELS[0]);
static int         g_micIndex   = 3;  // default 80%

static const unsigned long SLEEP_OPTIONS[] = {120000UL, 300000UL, 600000UL, 0UL}; // 2m, 5m, 10m, never
static const char *SLEEP_LABELS[] = {"2 min", "5 min", "10 min", "Never"};
static const int   SLEEP_COUNT  = 4;
static int         g_sleepIndex = 0;  // default 2 min

static const int   BRIGHT_LEVELS[] = {25, 64, 128, 192, 255};
static const int   BRIGHT_COUNT    = sizeof(BRIGHT_LEVELS) / sizeof(BRIGHT_LEVELS[0]);
static int         g_brightIndex   = 4;  // default 100%

#define FW_VERSION "1.0.0"

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

// ── WebSocket + audio state (declared early for menu access) ──────────────────

static volatile bool      g_wsConnected  = false;
static volatile bool      g_playing      = false;
static volatile bool      g_playTaskRun  = false;

#include "menu.h"

// ── Animated Face ─────────────────────────────────────────────────────────────
// Simple wizard/DM face that animates based on device state.
// States: IDLE (blinks), LISTENING (eyes wide), THINKING (eyes shift),
//         SPEAKING (mouth animates), SLEEPING (eyes closed)

enum FaceState { FACE_IDLE, FACE_LISTEN, FACE_THINK, FACE_SPEAK, FACE_SLEEP };
static volatile FaceState g_faceState = FACE_IDLE;
static volatile bool g_faceTaskRun = false;
static unsigned long g_lastBlink = 0;
static uint8_t g_mouthFrame = 0;
static uint8_t g_thinkFrame = 0;

// Colors
#define COL_SKIN    gfx->color565(220, 180, 140)
#define COL_HAT     gfx->color565(60, 40, 120)
#define COL_HATRIM  gfx->color565(180, 150, 50)
#define COL_EYE_W   RGB565_WHITE
#define COL_EYE_P   gfx->color565(40, 80, 40)
#define COL_MOUTH   gfx->color565(80, 20, 20)
#define COL_BEARD   gfx->color565(200, 200, 210)
#define COL_BG      RGB565_BLACK

// Draw the base face (static parts)
void drawFaceBase() {
  gfx->fillRect(0, 15, 128, 113, COL_BG);

  // Wizard hat
  gfx->fillTriangle(64, 20, 38, 58, 90, 58, COL_HAT);
  gfx->fillRect(30, 56, 68, 8, COL_HAT);
  // Hat brim highlight
  gfx->drawFastHLine(30, 63, 68, COL_HATRIM);
  gfx->drawFastHLine(32, 62, 64, COL_HATRIM);
  // Hat star
  gfx->fillCircle(62, 38, 3, COL_HATRIM);

  // Face oval
  gfx->fillRoundRect(40, 62, 48, 42, 12, COL_SKIN);

  // Beard
  gfx->fillTriangle(44, 90, 64, 120, 84, 90, COL_BEARD);
  gfx->fillRoundRect(42, 88, 44, 16, 6, COL_BEARD);
}

// Draw eyes based on state
void drawEyes(FaceState state, bool blinking) {
  // Clear eye area
  gfx->fillRect(46, 70, 36, 14, COL_SKIN);

  if (state == FACE_SLEEP || blinking) {
    // Closed eyes — horizontal lines
    gfx->drawFastHLine(49, 76, 8, COL_EYE_P);
    gfx->drawFastHLine(71, 76, 8, COL_EYE_P);
    return;
  }

  int xOff = 0;
  if (state == FACE_THINK) {
    // Eyes shift side to side
    xOff = (g_thinkFrame < 3) ? -2 : (g_thinkFrame < 6) ? 2 : 0;
  }

  // Eye whites
  int eyeW = (state == FACE_LISTEN) ? 7 : 6; // wider when listening
  gfx->fillRoundRect(49, 70 , eyeW * 2, 12, 4, COL_EYE_W);
  gfx->fillRoundRect(71, 70, eyeW * 2, 12, 4, COL_EYE_W);

  // Pupils
  int pSize = (state == FACE_LISTEN) ? 3 : 2;
  gfx->fillCircle(53 + xOff, 76, pSize, COL_EYE_P);
  gfx->fillCircle(75 + xOff, 76, pSize, COL_EYE_P);
}

// Draw mouth based on state
void drawMouth(FaceState state) {
  // Clear mouth area
  gfx->fillRect(52, 88, 24, 10, COL_SKIN);

  if (state == FACE_SPEAK) {
    // Animated mouth — alternates between open shapes
    switch (g_mouthFrame % 4) {
      case 0: gfx->fillRoundRect(56, 89, 16, 4, 2, COL_MOUTH); break;  // slightly open
      case 1: gfx->fillRoundRect(54, 88, 20, 8, 3, COL_MOUTH); break;  // wide open
      case 2: gfx->fillRoundRect(56, 89, 16, 5, 2, COL_MOUTH); break;  // medium
      case 3: gfx->fillRoundRect(58, 90, 12, 3, 1, COL_MOUTH); break;  // nearly closed
    }
  } else if (state == FACE_THINK) {
    // Wavy line — thinking
    gfx->drawFastHLine(56, 92, 16, COL_MOUTH);
    gfx->drawPixel(56 + (g_thinkFrame % 8) * 2, 91, COL_MOUTH);
  } else {
    // Neutral smile
    gfx->drawFastHLine(56, 92, 16, COL_MOUTH);
    gfx->drawPixel(55, 91, COL_MOUTH);
    gfx->drawPixel(72, 91, COL_MOUTH);
  }
}

void setFaceState(FaceState state) {
  g_faceState = state;
}

// Face animation task — runs on Core 1, updates ~10fps
static void faceTask(void *) {
  drawFaceBase();
  drawEyes(FACE_IDLE, false);
  drawMouth(FACE_IDLE);

  unsigned long lastFrame = 0;
  bool blinking = false;
  bool wasPlaying = false;

  while (g_faceTaskRun) {
    unsigned long now = millis();
    if (now - lastFrame < 100) { // ~10 fps
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    lastFrame = now;

    FaceState state = g_faceState;

    // Skip display updates during playback — SPI writes interfere with I2S DMA
    if (g_playTaskRun) {
      wasPlaying = true;
      // Still advance animation counters so it's smooth when we resume drawing
      if (state == FACE_SPEAK) g_mouthFrame++;
      if (state == FACE_THINK) g_thinkFrame = (g_thinkFrame + 1) % 9;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Redraw base after playback ends
    if (wasPlaying) {
      wasPlaying = false;
      drawFaceBase();
    }

    // Show/hide status text on state change
    static FaceState lastDrawnState = FACE_IDLE;
    if (state != lastDrawnState) {
      // Clear status area
      gfx->fillRect(0, 120, 128, 8, RGB565_BLACK);
      if (state == FACE_LISTEN) {
        gfx->setTextSize(1);
        gfx->setTextColor(RGB565_GREEN);
        gfx->setCursor(36, 120);
        gfx->print("Listening");
      }
      lastDrawnState = state;
    }

    // Blink logic (random blinks every 2-5 seconds)
    if (state != FACE_SLEEP && state != FACE_LISTEN) {
      if (!blinking && now - g_lastBlink > 2000 + (esp_random() % 3000)) {
        blinking = true;
        g_lastBlink = now;
      }
      if (blinking && now - g_lastBlink > 150) {
        blinking = false;
      }
    }

    drawEyes(state, blinking);
    drawMouth(state);

    // Advance animation frames
    if (state == FACE_SPEAK) g_mouthFrame++;
    if (state == FACE_THINK) g_thinkFrame = (g_thinkFrame + 1) % 9;

    vTaskDelay(pdMS_TO_TICKS(10));
  }
  vTaskDelete(nullptr);
}

void startFaceTask() {
  if (g_faceTaskRun) return;
  g_faceTaskRun = true;
  xTaskCreatePinnedToCore(faceTask, "face", 4096, nullptr, 1, nullptr, 1);
}

void stopFaceTask() {
  g_faceTaskRun = false;
  vTaskDelay(pdMS_TO_TICKS(100));
}

// ── NVS ───────────────────────────────────────────────────────────────────────

void loadPrefs() {
  Preferences prefs;
  prefs.begin("ailite", true);
  prefs.getString("workerUrl",     workerUrl,     sizeof(workerUrl));
  prefs.getString("apiKey",        apiKey,        sizeof(apiKey));
  prefs.getString("assistantName", assistantName, sizeof(assistantName));
  cachedIp  = prefs.getUInt("ip",  0);
  cachedGw  = prefs.getUInt("gw",  0);
  cachedSn  = prefs.getUInt("sn",  0);
  cachedDns = prefs.getUInt("dns", 0);

  // Load multi-WiFi
  g_wifiCount = prefs.getInt("wifiCount", 0);
  if (g_wifiCount > MAX_WIFI_NETS) g_wifiCount = MAX_WIFI_NETS;
  for (int i = 0; i < g_wifiCount; i++) {
    char keyS[12], keyP[12];
    snprintf(keyS, sizeof(keyS), "wSSID%d", i);
    snprintf(keyP, sizeof(keyP), "wPASS%d", i);
    prefs.getString(keyS, g_wifiNets[i].ssid, sizeof(g_wifiNets[i].ssid));
    prefs.getString(keyP, g_wifiNets[i].pass, sizeof(g_wifiNets[i].pass));
  }
  g_wifiActive = prefs.getInt("wifiActive", 0);
  if (g_wifiActive >= g_wifiCount) g_wifiActive = 0;

  // Load settings
  g_volIndex    = prefs.getInt("volIndex",  3);
  g_micIndex    = prefs.getInt("micIndex",  3);
  g_sleepIndex  = prefs.getInt("sleepIdx",  0);
  g_brightIndex = prefs.getInt("brightIdx", 4);

  // Migrate old single-WiFi format
  if (g_wifiCount == 0) {
    char oldSsid[64] = "", oldPass[64] = "";
    prefs.getString("wifiSsid", oldSsid, sizeof(oldSsid));
    prefs.getString("wifiPass", oldPass, sizeof(oldPass));
    if (strlen(oldSsid) > 0) {
      strlcpy(g_wifiNets[0].ssid, oldSsid, sizeof(g_wifiNets[0].ssid));
      strlcpy(g_wifiNets[0].pass, oldPass, sizeof(g_wifiNets[0].pass));
      g_wifiCount = 1;
      g_wifiActive = 0;
    }
  }

  prefs.end();
}

void savePrefs() {
  Preferences prefs;
  prefs.begin("ailite", false);
  prefs.putString("workerUrl", workerUrl);
  prefs.putString("apiKey",    apiKey);

  // Save multi-WiFi
  prefs.putInt("wifiCount", g_wifiCount);
  for (int i = 0; i < g_wifiCount; i++) {
    char keyS[12], keyP[12];
    snprintf(keyS, sizeof(keyS), "wSSID%d", i);
    snprintf(keyP, sizeof(keyP), "wPASS%d", i);
    prefs.putString(keyS, g_wifiNets[i].ssid);
    prefs.putString(keyP, g_wifiNets[i].pass);
  }
  prefs.putInt("wifiActive", g_wifiActive);

  // Save settings
  prefs.putInt("volIndex",  g_volIndex);
  prefs.putInt("micIndex",  g_micIndex);
  prefs.putInt("sleepIdx",  g_sleepIndex);
  prefs.putInt("brightIdx", g_brightIndex);

  prefs.end();
}

// ── Sleep ─────────────────────────────────────────────────────────────────────

void goSleep() {
  showMsg("ZZZ", "press A to wake", RGB565_BLUE);
  delay(700);
  gfx->fillScreen(RGB565_BLACK);
  analogWrite(PIN_LCD_BL, 0);
  digitalWrite(PIN_SPKR_EN, LOW);
  rtc_gpio_hold_en((gpio_num_t)PIN_PWR_CTL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_A, 0);
  esp_deep_sleep_start();
}

// ── Config portal ─────────────────────────────────────────────────────────────

void openPortal() {
  showMsg("Setup", "SoloRolls-DM", RGB565_YELLOW);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("SoloRolls-DM");
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
    // Add/update WiFi network
    const char *newSsid = doc["ssid"] | "";
    const char *newPass = doc["password"] | "";
    if (strlen(newSsid) > 0) {
      // Check if SSID already exists — update password
      int idx = -1;
      for (int i = 0; i < g_wifiCount; i++) {
        if (strcmp(g_wifiNets[i].ssid, newSsid) == 0) { idx = i; break; }
      }
      if (idx < 0 && g_wifiCount < MAX_WIFI_NETS) {
        idx = g_wifiCount++;
      } else if (idx < 0) {
        idx = MAX_WIFI_NETS - 1; // overwrite last slot if full
      }
      strlcpy(g_wifiNets[idx].ssid, newSsid, sizeof(g_wifiNets[idx].ssid));
      strlcpy(g_wifiNets[idx].pass, newPass, sizeof(g_wifiNets[idx].pass));
      g_wifiActive = idx;
    }
    strlcpy(workerUrl, doc["workerUrl"] | workerUrl, sizeof(workerUrl));
    strlcpy(apiKey,    doc["apiKey"]    | apiKey,    sizeof(apiKey));
    savePrefs();
    Preferences p; p.begin("ailite", false);
    p.putUInt("ip", 0); p.putUInt("gw", 0); p.putUInt("sn", 0); p.putUInt("dns", 0);
    p.end();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    delay(500);
    ESP.restart();
  });

  server.on("/remove", HTTP_POST, [&]() {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      server.send(400, "text/plain", "bad json"); return;
    }
    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= g_wifiCount) {
      server.send(400, "text/plain", "invalid index"); return;
    }
    // Shift remaining entries down
    for (int i = idx; i < g_wifiCount - 1; i++) {
      memcpy(&g_wifiNets[i], &g_wifiNets[i + 1], sizeof(WifiEntry));
    }
    g_wifiCount--;
    if (g_wifiActive >= g_wifiCount) g_wifiActive = max(0, g_wifiCount - 1);
    if (g_wifiActive == idx) g_wifiActive = 0;
    savePrefs();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.onNotFound([&]() {
    int n = WiFi.scanNetworks();
    String opts = "";
    String activeSsid = (g_wifiActive >= 0 && g_wifiActive < g_wifiCount) 
                        ? String(g_wifiNets[g_wifiActive].ssid) : "";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      String sel = (ssid == activeSsid) ? " selected" : "";
      opts += "<option value=\"" + ssid + "\"" + sel + ">" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>\n";
    }
    WiFi.scanDelete();

    // Build saved networks HTML
    String savedHtml = "";
    if (g_wifiCount == 0) {
      savedHtml = "<p class=\"empty-msg\">No saved networks</p>";
    } else {
      for (int i = 0; i < g_wifiCount; i++) {
        savedHtml += "<div class=\"saved-net\"><span class=\"name\">";
        savedHtml += String(g_wifiNets[i].ssid);
        if (i == g_wifiActive) savedHtml += "<span class=\"active\">(active)</span>";
        savedHtml += "</span><button class=\"btn-danger\" onclick=\"removeNet(";
        savedHtml += String(i);
        savedHtml += ")\">Remove</button></div>";
      }
    }

    String html = String(PORTAL_HTML);
    html.replace("{{WIFI_OPTIONS}}", opts);
    html.replace("{{SAVED_NETS}}",   savedHtml);
    html.replace("{{WORKER_URL}}",   String(workerUrl));
    html.replace("{{API_KEY}}",      String(apiKey));
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

// ── Downsample: 24kHz stereo → 16kHz mono (3:2 ratio, linear interp) ─────────

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
  i2s.setInputVolume(MIC_LEVELS[g_micIndex]);
}

void codecBeginPlay() {
  AudioInfo info(48000, 2, REC_BITS);  // 48kHz stereo (stable I2S clock)
  auto c          = i2s.defaultConfig(TX_MODE);
  c.copyFrom(info);
  c.output_device = DAC_OUTPUT_ALL;
  c.is_master     = true;
  c.mclk_multiple = 256;
  i2s.begin(c);
  i2s.setVolume(VOL_LEVELS[g_volIndex]);
}

// ── WebSocket + audio state ───────────────────────────────────────────────────

static SemaphoreHandle_t  g_mutex       = nullptr;
static WebSocketsClient   g_ws;
static volatile bool      g_wsTaskRun   = false;

// Audio playback ring buffer (receives 24kHz PCM from server)
#define RING_SIZE       (96 * 1024)   // 96 KB — ~2 seconds of 24kHz mono 16-bit
#define PREBUFFER_BYTES (6 * 1024)    // start playback after 6KB buffered (~125ms)

static uint8_t  *g_ring         = nullptr;
static volatile size_t g_ringHead    = 0;
static volatile size_t g_ringTail    = 0;
static volatile bool   g_ringReady   = false;
static volatile bool   g_interrupted = false;

static size_t ringAvailable() {
  size_t h = g_ringHead, t = g_ringTail;
  return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}

static size_t ringFree() {
  return RING_SIZE - 1 - ringAvailable();
}

static void ringWrite(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    g_ring[g_ringHead] = data[i];
    g_ringHead = (g_ringHead + 1) % RING_SIZE;
  }
}

static size_t ringRead(uint8_t *dst, size_t maxLen) {
  size_t avail = ringAvailable();
  size_t toRead = (avail < maxLen) ? avail : maxLen;
  for (size_t i = 0; i < toRead; i++) {
    dst[i] = g_ring[g_ringTail];
    g_ringTail = (g_ringTail + 1) % RING_SIZE;
  }
  return toRead;
}

static inline void hwLock()   { xSemaphoreTake(g_mutex, portMAX_DELAY); }
static inline void hwUnlock() { xSemaphoreGive(g_mutex); }

// Mic pause flag — play task sets this to claim the I2S bus
static volatile bool g_micPaused  = false;

// ── Playback task: drains ring buffer to I2S (Core 1) ─────────────────────────
// Gemini sends 24kHz mono PCM. We upsample to 48kHz stereo for the ES8311.
// 24kHz→48kHz = 2x (duplicate each sample), mono→stereo = 2x. Total: 4x expansion.

static void playTask(void *) {
  uint8_t playChunk[256];     // mono 24kHz PCM from ring buffer (128 samples)
  int16_t stereoOut[512];     // 48kHz stereo output (128 input → 256 output frames × 2ch)
  unsigned long lastDataTime = 0;

  // Pause mic — we share the I2S bus
  g_micPaused = true;
  vTaskDelay(pdMS_TO_TICKS(120)); // let mic task release I2S

  // Wait for prebuffer
  while (g_playTaskRun && !g_ringReady) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  if (g_playTaskRun && !g_playing) {
    codecBeginPlay();
    digitalWrite(PIN_SPKR_EN, HIGH);
    g_playing = true;
  }

  lastDataTime = millis();

  while (g_playTaskRun) {
    if (g_interrupted) break;
    size_t avail = ringAvailable();
    if (avail > 0) {
      lastDataTime = millis();
      // Read mono 24kHz PCM (max 256 bytes = 128 samples)
      size_t n = ringRead(playChunk, min(avail, sizeof(playChunk)));
      size_t monoSamples = n / 2;
      const int16_t *mono = (const int16_t *)playChunk;
      // Upsample 24kHz→48kHz (2x) + mono→stereo
      // Each input sample becomes 2 stereo frames (4 output samples)
      for (size_t i = 0; i < monoSamples; i++) {
        int16_t s = mono[i];
        stereoOut[i * 4]     = s; // frame 1 L
        stereoOut[i * 4 + 1] = s; // frame 1 R
        stereoOut[i * 4 + 2] = s; // frame 2 L (duplicate for 2x upsample)
        stereoOut[i * 4 + 3] = s; // frame 2 R
      }
      // Write: monoSamples * 4 samples * 2 bytes = monoSamples * 8 bytes
      i2s.write((uint8_t *)stereoOut, monoSamples * 8);
    } else {
      // Buffer empty — wait briefly for more data, then exit
      if (millis() - lastDataTime > 500) break;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  // Clean up
  if (g_playing) {
    i2s.end();
    digitalWrite(PIN_SPKR_EN, LOW);
    g_playing = false;
  }
  // Unpause mic — it will reinitialize I2S in RX mode
  g_micPaused = false;
  setFaceState(FACE_LISTEN);
  g_playTaskRun = false;
  vTaskDelete(nullptr);
}

static void startPlayback() {
  if (g_playTaskRun) return;  // already running, don't reset
  g_ringHead    = 0;
  g_ringTail    = 0;
  g_ringReady   = false;
  g_interrupted = false;
  g_playing     = false;
  g_playTaskRun = true;
  xTaskCreatePinnedToCore(playTask, "play", 8192, nullptr, 3, nullptr, 1);
}

static void stopPlayback() {
  g_interrupted = true;
  g_playTaskRun = false;
  vTaskDelay(pdMS_TO_TICKS(100));
  // Task cleans up g_playing/i2s/speaker on exit
  g_playing = false;
}

// ── WebSocket event handler ───────────────────────────────────────────────────

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      g_wsConnected = true;
      break;
    case WStype_DISCONNECTED:
      g_wsConnected = false;
      break;
    case WStype_BIN:
      // Raw 24kHz mono PCM audio from Gemini via server
      if (!g_playTaskRun) startPlayback();
      if (length <= ringFree()) {
        ringWrite(payload, length);
      }
      if (!g_ringReady && ringAvailable() >= PREBUFFER_BYTES) {
        g_ringReady = true;
      }
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length) != DeserializationError::Ok) break;
      const char *t = doc["type"] | "";
      if (strcmp(t, "transcript") == 0) {
        // Player's words transcribed — we're listening
        setFaceState(FACE_LISTEN);
      } else if (strcmp(t, "response") == 0) {
        // DM response text arrived — face should be speaking
        setFaceState(FACE_SPEAK);
      } else if (strcmp(t, "interrupted") == 0) {
        // Gemini interrupted itself (player started talking)
        g_interrupted = true;
        setFaceState(FACE_LISTEN);
      } else if (strcmp(t, "error") == 0) {
        stopFaceTask();
        showMsg("ERR", doc["message"] | "?", RGB565_RED);
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

// ── Connect WebSocket ─────────────────────────────────────────────────────────

bool connectWS() {
  String wsUrl = String(workerUrl);
  bool isSecure = wsUrl.startsWith("https://");
  if      (isSecure)                    wsUrl = "wss://" + wsUrl.substring(8);
  else if (wsUrl.startsWith("http://")) wsUrl = "ws://"  + wsUrl.substring(7);
  while (wsUrl.endsWith("/")) wsUrl = wsUrl.substring(0, wsUrl.length() - 1);
  wsUrl += "/ws?key=";
  wsUrl += apiKey;

  String noScheme = wsUrl.substring(isSecure ? 6 : 5);
  int slash = noScheme.indexOf('/');
  String wsHost = noScheme.substring(0, slash);
  String wsPath = noScheme.substring(slash);

  g_ws.onEvent(onWsEvent);
  if (isSecure) g_ws.beginSSL(wsHost, 443, wsPath);
  else          g_ws.begin(wsHost, 80, wsPath);
  g_ws.setReconnectInterval(5000);
  g_ws.enableHeartbeat(15000, 5000, 2);  // ping every 15s, 5s pong timeout, drop after 2 missed

  g_wsTaskRun = true;
  xTaskCreatePinnedToCore(wsTask, "ws", 8192, nullptr, 2, nullptr, 0);

  unsigned long t = millis();
  while (!g_wsConnected && millis() - t < 10000) delay(20);
  return g_wsConnected;
}

void disconnectWS() {
  g_wsTaskRun = false;
  vTaskDelay(pdMS_TO_TICKS(50));
  g_ws.disconnect();
  g_wsConnected = false;
}

// ── Continuous mic streaming task (Core 1) ────────────────────────────────────
// Streams 16kHz mono PCM to server continuously. Gemini's VAD handles turn-taking.

static volatile bool g_micTaskRun = false;

static void micTask(void *) {
  uint8_t chunk[512];
  int16_t dsOut[256];
  uint8_t stereoAccum[512 * 6];
  size_t accumLen = 0;

  codecBeginRec();
  setFaceState(FACE_LISTEN);

  while (g_micTaskRun) {
    // Pause mic during playback — release I2S bus
    if (g_micPaused) {
      i2s.end();
      while (g_micPaused && g_micTaskRun) vTaskDelay(pdMS_TO_TICKS(50));
      if (!g_micTaskRun) break;
      accumLen = 0;
      codecBeginRec();
      setFaceState(FACE_LISTEN);
      continue;
    }

    if (!g_wsConnected) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    size_t n = i2s.readBytes(chunk, sizeof(chunk));
    if (n > 0) {
      memcpy(stereoAccum + accumLen, chunk, n);
      accumLen += n;

      size_t frameSize = REC_CH_HW * (REC_BITS / 8);
      size_t accumFrames = accumLen / frameSize;
      size_t usableFrames = (accumFrames / 3) * 3;

      if (usableFrames >= 3) {
        size_t outSamples = downsample(
          (const int16_t *)stereoAccum, usableFrames, dsOut
        );
        size_t pcmLen = outSamples * sizeof(int16_t);

        hwLock();
        g_ws.sendBIN((uint8_t *)dsOut, pcmLen);
        hwUnlock();

        size_t usedBytes = usableFrames * frameSize;
        size_t remaining = accumLen - usedBytes;
        if (remaining > 0) memmove(stereoAccum, stereoAccum + usedBytes, remaining);
        accumLen = remaining;
      }
    }
  }

  i2s.end();
  vTaskDelete(nullptr);
}

void startMicStream() {
  if (g_micTaskRun) return;
  g_micTaskRun = true;
  xTaskCreatePinnedToCore(micTask, "mic", 8192, nullptr, 2, nullptr, 1);
}

void stopMicStream() {
  g_micTaskRun = false;
  vTaskDelay(pdMS_TO_TICKS(100));
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  rtc_gpio_hold_dis((gpio_num_t)PIN_PWR_CTL);
  pinMode(PIN_PWR_CTL,  OUTPUT); digitalWrite(PIN_PWR_CTL,  HIGH);
  pinMode(PIN_BTN_A,    INPUT_PULLUP);
  pinMode(PIN_BTN_B,    INPUT_PULLUP);
  pinMode(PIN_SPKR_EN,  OUTPUT); digitalWrite(PIN_SPKR_EN,  LOW);
  pinMode(PIN_LCD_BL,   OUTPUT); analogWrite(PIN_LCD_BL, 255);

  Serial.begin(115200);
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

  g_ring = (uint8_t *)ps_malloc(RING_SIZE);
  if (!g_ring) { showMsg("PSRAM", "fail", RGB565_RED); while (true) delay(1000); }

  myPins.addI2C(PinFunction::CODEC, I2C_SCL, I2C_SDA);
  myPins.addI2S(PinFunction::CODEC, I2S_MCLK, I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN);

  g_mutex = xSemaphoreCreateMutex();
  loadPrefs();
  showHeader();

  // B held at boot → config portal
  delay(50);
  if (digitalRead(PIN_BTN_B) == LOW) {
    openPortal();
    goSleep();
    return;
  }

  // Missing creds → config portal
  if (g_wifiCount == 0 || !strlen(workerUrl) || !strlen(apiKey)) {
    showMsg("No config", "hold B to setup", RGB565_RED);
    delay(2000);
    openPortal();
    goSleep();
    return;
  }

  // Connect WiFi — try active network first, then others
  showMsg("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  bool wifiOk = false;
  wl_status_t lastStatus = WL_IDLE_STATUS;
  // Try active network first with cached IP
  if (g_wifiActive >= 0 && g_wifiActive < g_wifiCount) {
    if (cachedIp) {
      WiFi.config(IPAddress(cachedIp), IPAddress(cachedGw), IPAddress(cachedSn), IPAddress(cachedDns));
    } else {
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE); // ensure DHCP
    }
    WiFi.begin(g_wifiNets[g_wifiActive].ssid, g_wifiNets[g_wifiActive].pass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) delay(100);
    lastStatus = (wl_status_t)WiFi.status();
    if (lastStatus == WL_CONNECTED) wifiOk = true;
  }
  // Try remaining networks (including active again with clean DHCP)
  if (!wifiOk) {
    for (int i = 0; i < g_wifiCount && !wifiOk; i++) {
      WiFi.disconnect(true);
      delay(100);
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE); // clean DHCP
      WiFi.begin(g_wifiNets[i].ssid, g_wifiNets[i].pass);
      unsigned long t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(100);
      lastStatus = (wl_status_t)WiFi.status();
      if (lastStatus == WL_CONNECTED) {
        g_wifiActive = i;
        wifiOk = true;
      }
    }
  }

  if (!wifiOk) {
    const char *reason = "unknown";
    switch (lastStatus) {
      case WL_NO_SSID_AVAIL:    reason = "SSID not found"; break;
      case WL_CONNECT_FAILED:   reason = "connect failed"; break;
      case WL_CONNECTION_LOST:  reason = "connection lost"; break;
      case WL_DISCONNECTED:     reason = "disconnected"; break;
      case WL_NO_SHIELD:        reason = "no WiFi hw"; break;
      case WL_IDLE_STATUS:      reason = "no networks"; break;
      default:                  reason = "timeout"; break;
    }
    showMsg("WiFi fail", reason, RGB565_RED);
    // Wait up to 5s — press B to open config portal, otherwise sleep
    unsigned long t = millis();
    bool portalRequested = false;
    while (millis() - t < 5000) {
      if (digitalRead(PIN_BTN_B) == LOW) {
        delay(50);
        if (digitalRead(PIN_BTN_B) == LOW) { portalRequested = true; break; }
      }
      delay(50);
    }
    if (portalRequested) {
      openPortal();
    }
    goSleep();
    return;
  }

  // Cache IP
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
  fetchName();

  // Connect WebSocket
  showMsg("Connect...");
  if (!connectWS()) {
    showMsg("WS fail", nullptr, RGB565_RED);
    delay(2000);
    goSleep();
    return;
  }

  // Wait for first button press to begin the session
  showMsg("Ready", "press A: begin", RGB565_GREEN);
  while (digitalRead(PIN_BTN_A) == HIGH) {
    if (digitalRead(PIN_BTN_B) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_B) == LOW) {
        disconnectWS();
        openPortal();
        goSleep();
        return;
      }
    }
    delay(20);
  }
  // Wait for release
  while (digitalRead(PIN_BTN_A) == LOW) delay(10);
  delay(50);

  // Start face animation and continuous mic streaming
  startFaceTask();
  setFaceState(FACE_LISTEN);
  startMicStream();

  // Apply brightness setting
  analogWrite(PIN_LCD_BL, BRIGHT_LEVELS[g_brightIndex]);

  // Main loop: hold A + tap B → settings menu, idle timeout
  unsigned long lastActivity = millis();
  while (true) {
    // Settings menu: A held + B tapped
    if (digitalRead(PIN_BTN_A) == LOW && digitalRead(PIN_BTN_B) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_A) == LOW && digitalRead(PIN_BTN_B) == LOW) {
        waitBothReleased();
        // Pause mic and face during menu
        stopMicStream();
        stopFaceTask();
        bool needPortal = openSettingsMenu();
        // Handle new game reconnection
        if (g_needReconnect) {
          g_needReconnect = false;
          disconnectWS();
          showMsg("Connecting...");
          if (!connectWS()) {
            showMsg("WS fail", nullptr, RGB565_RED);
            delay(2000);
            break;
          }
        }
        // Restore display
        showHeader();
        startFaceTask();
        setFaceState(FACE_LISTEN);
        startMicStream();
        lastActivity = millis();
        if (needPortal) {
          stopMicStream();
          stopPlayback();
          stopFaceTask();
          disconnectWS();
          openPortal();
          break;
        }
        continue;
      }
    }

    // B held alone → config portal (legacy shortcut)
    if (digitalRead(PIN_BTN_B) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_B) == LOW && digitalRead(PIN_BTN_A) == HIGH) {
        // Wait to see if it's a long hold (>1s)
        unsigned long holdStart = millis();
        while (digitalRead(PIN_BTN_B) == LOW && millis() - holdStart < 1000) delay(10);
        if (millis() - holdStart >= 1000) {
          stopMicStream();
          stopPlayback();
          stopFaceTask();
          disconnectWS();
          openPortal();
          break;
        }
      }
    }

    // Track activity — reset timeout when audio is playing or mic is active
    if (g_playTaskRun || g_faceState == FACE_SPEAK || g_faceState == FACE_THINK) {
      lastActivity = millis();
    }

    // Sleep after extended idle (respects sleep timer setting)
    unsigned long sleepMs = SLEEP_OPTIONS[g_sleepIndex];
    if (sleepMs > 0 && millis() - lastActivity > sleepMs) break;

    delay(50);
  }

  stopMicStream();
  stopPlayback();
  stopFaceTask();
  disconnectWS();
  goSleep();
}

void loop() {}
