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

Key libs: `arduino-audio-tools`, `arduino-audio-driver`, `Arduino_GFX_Library`, `WiFiManager`, `ArduinoJson`, `gilmaimon/ArduinoWebsockets`.

**NVS (Preferences):** stores `workerUrl` and `apiKey` under namespace `"ailite"`. No hardcoded config.

**Captive portal:** WiFiManager with two custom params (Worker URL, API Key). Opened on first boot or when Button A held at boot. Portal SSID: "AI-Lite-Setup", timeout 300 s.

**Boot flow (wake from Button B / `ESP_SLEEP_WAKEUP_EXT0`):** `WiFi.begin()` with saved creds → WebSocket connect (`ws://` or `wss://` derived from workerUrl) → codec RX_MODE 24kHz stereo → record while Button B held → downsample 24kHz stereo→16kHz mono → build WAV header → send WAV in 1kB chunks → send `{"type":"done"}` → wait for pipeline messages → codec TX_MODE for playback → stream MP3 binary frames through `EncodedAudioOutput`+`MP3DecoderHelix` → `audio_end` received → wait 10 s → deep sleep.

**Boot flow (first boot / reset):** Button A held OR missing NVS credentials → `openPortal()` → save NVS → deep sleep. Credentials present and A not held → deep sleep immediately (wake via Button B).

**Button mapping:** A = hold on boot to reconfigure; B = hold to start voice turn (wake source).

**WebSocket URL:** constructed from `workerUrl` by replacing `https://`→`wss://`, `http://`→`ws://`, appending `/ws?key=<apiKey>`. `ws.setInsecure()` used for wss connections.

`EncodedAudioOutput` + `MP3DecoderHelix` handles MP3 decode. Codec switches between `RX_MODE` (record) and `TX_MODE` (play) via separate `i2s.begin()` calls.

## Hardware reference

The `aipilite-examples` repo lives at `/Users/konsumer/Documents/dev/aipilite-examples/`. Battery and sound examples there are the reference implementations for sleep/wake and audio I/O.

## Deploy sequence

```bash
npx wrangler kv namespace create CONFIG   # paste ID into wrangler.toml
npx wrangler secret put API_SECRET
npm run deploy
```
