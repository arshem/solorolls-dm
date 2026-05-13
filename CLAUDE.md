# ailite-cf

Cloudflare Worker + ESP32 Arduino firmware for AI voice assistant on the AI-Lite (ESP32-S3) device.

## Stack

- **Worker**: plain ESM JS, no build step, `wrangler deploy`
- **Firmware**: Arduino/PlatformIO, C++, targets ESP32-S3
- **No TypeScript** in either side

## Worker (`worker/src/index.js`)

**Auth model:** `API_SECRET` wrangler secret = admin only (user management). User keys stored in KV as `user:<key>` → JSON config. `getUser()` looks up key; `isMaster()` checks against `API_SECRET`.

**KV structure:** `user:<key>` → `{name, personality, ttsModel, ttsLang, ttsVoice}`

**Endpoints:**
- `GET /` — self-contained UI (no auth, inline HTML/CSS/JS, 37 Aura-2 voices, admin panel)
- `GET /whoami` — returns `{name, isAdmin}` for the authenticated key
- `POST /chat` — dual mode: WAV binary in → MP3 stream out (device); JSON `{text}` in → `{text, audio}` base64 out (UI)
- `GET/POST /config` — per-user config; POST blocked for admin (no personal config)
- `GET/POST/DELETE /admin/keys` — user management, master auth only

**TTS dispatch (`runTTS`):** `aura-2-en`/`aura-2-es` → `@cf/deepgram/aura-2-*`, returns `ReadableStream`, piped directly to response for device. `melotts` → `@cf/myshell-ai/melotts`, returns base64, wrapped in ReadableStream for device.

**STT language** derived from `ttsModel` (aura-2-es → "es", everything else → `ttsLang`).

**Config fields:** `name`, `personality`, `ttsModel` (`melotts`|`aura-2-en`|`aura-2-es`), `ttsVoice` (Aura-2 speaker), `ttsLang` (MeloTTS lang code).

## Firmware (`firmware/src/main.cpp`)

Hardware: AI-Lite = ESP32-S3, ES8311 codec (I2C + I2S), ST7735 LCD, 8MB PSRAM, two buttons.
Pin definitions in `firmware/src/pins.h` — don't change without checking hardware.

Key libs: `arduino-audio-tools`, `arduino-audio-driver`, `Arduino_GFX_Library`, `WiFiManager`, `ArduinoJson`.

**NVS (Preferences):** stores `workerUrl` and `apiKey` under namespace `"ailite"`. No hardcoded config.

**Captive portal:** WiFiManager with two custom params (Worker URL, API Key). Button B reopens portal anytime via `reopenPortal()`.

**Boot flow:** deep sleep → button A wakes → WiFiManager connects → fetch `/config` (populates `DeviceConfig cfg`) → show `cfg.name` → hold A to record → POST WAV → play response → sleep after 30s idle.

**`DeviceConfig`:** populated from `GET /config` JSON. `cfg.isStreaming = (ttsModel starts with "aura-2")`.

**Playback modes:**
- `cfg.isStreaming = true` → `playStreaming()`: feeds HTTP chunks directly into helix → I2S in real time
- `cfg.isStreaming = false` → `playBuffered()`: downloads full MP3 into PSRAM, then decodes

`EncodedAudioOutput` + `MP3DecoderHelix` handles decode in both paths. Codec init at 24kHz stereo; helix `setAudioInfo` callback adjusts if actual rate differs.

## Hardware reference

The `aipilite-examples` repo lives at `/Users/konsumer/Documents/dev/aipilite-examples/`. Battery and sound examples there are the reference implementations for sleep/wake and audio I/O.

## Deploy sequence

```bash
npx wrangler kv namespace create CONFIG   # paste ID into wrangler.toml
npx wrangler secret put API_SECRET
npm run deploy
```
