# ailite-cf

Cloudflare Worker + ESP32 Arduino firmware for AI voice assistant on the AI-Lite (ESP32-S3) device.

## Stack

- **Worker**: plain ESM JS, no build step, `wrangler deploy`
- **Firmware**: Arduino/PlatformIO, C++, targets ESP32-S3
- **No TypeScript** in either side

## Worker (`worker/worker.js`)

**Auth model:** `API_SECRET` wrangler secret = admin only (user management). User keys stored in KV as `user:<key>` → JSON config. `getUserByKey(key, env)` core lookup; `getUser(request, env)` extracts from `Authorization: Bearer`; `isMaster()` checks against `API_SECRET`.

**KV structure:** `user:<key>` → `{name, personality, ttsModel, ttsVoice}`

**Endpoints:**
- `GET /` — web UI (served from `worker/public/`, no auth)
- `GET /whoami` — returns `{name, isAdmin}` for authenticated key
- `POST /chat` — dual mode: WAV binary → MP3 stream (device); JSON `{text}` → `{text, audio}` base64 (UI); WAV binary + `Accept: application/json` → `{input, text, audio}` (browser mic)
- `GET/POST /config` — per-user config; POST blocked for admin
- `GET/POST/PUT/DELETE /admin/keys` — user management, master auth only
- `GET /ws?key=<key>` — WebSocket upgrade; device audio streaming (see below)

**WebSocket protocol (`/ws`):** auth via `?key=` query param. Binary frames = audio chunks from device. Text frames are JSON:
- Client→Server: `{"type":"done"}` — button released, triggers STT→LLM→TTS pipeline
- Server→Client: `{"type":"transcript","text":"..."}` — Whisper result
- Server→Client: `{"type":"response","text":"..."}` — LLM result
- Server→Client: `{"type":"audio_start"}`, binary MP3 frames, `{"type":"audio_end"}` — streaming TTS
- Server→Client: `{"type":"error","message":"..."}` — pipeline error
Connection stays open for multi-turn conversation.

**TTS dispatch (`runTTS`):** `aura-2-en`/`aura-2-es` → `@cf/deepgram/aura-2-*`, returns `ReadableStream`. `melotts` (legacy, not in UI) → `@cf/myshell-ai/melotts`, returns base64 wrapped in stream.

**STT language** derived from `ttsModel` (aura-2-es → "es", else "en").

**Config fields:** `name`, `personality`, `ttsModel` (`aura-2-en`|`aura-2-es`; `melotts` still works via API), `ttsVoice` (Aura-2 speaker name).

**Voices:** `aura-2-en` has 39 voices (default: luna); `aura-2-es` has 10 voices (default: aquila). Lists in `worker/public/index.js` `VOICES` object.

## Firmware (`firmware/src/main.cpp`)

Hardware: AI-Lite = ESP32-S3, ES8311 codec (I2C + I2S), ST7735 LCD, 8MB PSRAM, two buttons.
Pin definitions in `firmware/src/pins.h` — don't change without checking hardware.

Key libs: `arduino-audio-tools`, `arduino-audio-driver`, `Arduino_GFX_Library`, `WebServer`, `DNSServer`, `HTTPClient`, `ArduinoJson`, `gilmaimon/ArduinoWebsockets`.

**NVS (Preferences):** namespace `"ailite"` stores: `wifiSsid`, `wifiPass`, `workerUrl`, `apiKey`, `assistantName` (cached from `/config`), `ip`/`gw`/`sn`/`dns` (cached DHCP for fast reconnect).

**Captive portal:** custom `WebServer`+`DNSServer` on AP `"AI-Lite-Setup"`. HTML served from `portal.h`. Scans networks, shows dropdown. POST `/save` receives JSON `{ssid, password, workerUrl, apiKey}`, writes NVS, clears IP cache, restarts. Timeout 300 s.

**Button mapping:** A (GPIO 1, RTC-capable) = wake source + hold to record; B (GPIO 42, not RTC) = config portal trigger (while awake only).

**Boot flow (power-on / reset):**
1. B held → `openPortal()` → `goSleep()`
2. Missing creds → show message → `openPortal()` → `goSleep()`
3. Woke from deep sleep (`ESP_RST_DEEPSLEEP`) → `doVoiceTurn()` → idle loop
4. Otherwise → idle loop

**Idle loop:** A held → `doVoiceTurn()` (resets timeout); B held → `openPortal()`; timeout (`SLEEP_AFTER_MS`) → `goSleep()`.

**`doVoiceTurn()` flow:**
1. `WiFi.begin(ssid, pass)` — async; uses cached IP to skip DHCP if available
2. `codecBeginRec()` immediately — recording starts while WiFi connects
3. Record into PSRAM `rawBuf` while A held; `i2s.end()` on release
4. Wait for WiFi (up to 15 s); on first connect, save DHCP values to NVS
5. Connect WebSocket (`wss://` from `https://` workerUrl, `/ws?key=<apiKey>`); WS task spawned on Core 0
6. Downsample 24kHz stereo→16kHz mono; build WAV header; send in 1kB chunks; send `{"type":"done"}`
7. Wait for `audio_end` (or A interrupt); drain MP3 DMA buffer; `stopAudio()`
8. Stop WS task; `fetchName()` (GET `/config`, updates `assistantName` in NVS + header if changed)

**WS task (Core 0):** runs `g_ws.loop()` under `g_mutex` every 2 ms. `onWsEvent` handles binary (feed to `EncodedAudioOutput`+`MP3DecoderHelix`) and JSON messages (update display, start/stop codec, set `g_pipeDone`).

**`showHeader()`:** redraws top bar (y 0–14) with `assistantName`. Called at boot and when name changes.

**Sleep:** `esp_sleep_enable_ext0_wakeup(PIN_BTN_A, 0)` — GPIO 1 is RTC-capable. `rtc_gpio_hold_en(PIN_PWR_CTL)` keeps power rail active during sleep.

**WebSocket URL:** `https://` → `wss://`, `http://` → `ws://`, append `/ws?key=<apiKey>`. `beginSSL()` skips cert verification by default.

`EncodedAudioOutput` + `MP3DecoderHelix` handles MP3 decode. Codec switches between `RX_MODE` (record) and `TX_MODE` (play) via separate `i2s.begin()` calls.

## Hardware reference

The `aipilite-examples` repo lives at `/Users/konsumer/Documents/dev/aipilite-examples/`. Battery and sound examples there are the reference implementations for sleep/wake and audio I/O.

## Deploy sequence

```bash
npx wrangler kv namespace create CONFIG   # paste ID into wrangler.toml
npx wrangler secret put API_SECRET
npm run deploy
```
