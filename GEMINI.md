# SoloRolls-DM 

Solo D&D Dungeon Master: ESP32-S3 device + Python server + Gemini Live API for real-time bidirectional voice conversation.

## Stack

- **Server**: FastAPI/Python, Gemini Live API (`gemini-3.1-flash-live-preview`), no separate STT/LLM/TTS
- **Firmware**: Arduino/PlatformIO, C++, targets ESP32-S3 (AI-Lite)
- **Web UI**: vanilla HTML/JS/CSS in `worker/public/`, served by the Python server
- **Deploy**: Docker (docker-compose) or direct `python local/server.py`

## Server (`local/server.py`)

**Auth model:** `API_SECRET` env var = admin key. User keys stored in `users.json`. `get_user(key, users)` core lookup; `bearer(request)` extracts from `Authorization: Bearer`; admin check against `API_SECRET`.

**User store:** `local/users.json` — flat JSON dict of `key → {name, personality, voice}`.

**Session store:** `local/sessions/<user_key>.json` — conversation history with timestamp. Auto-expires after 1 hour of inactivity.

**Endpoints:**
- `GET /` — web UI (static files from `worker/public/`)
- `GET /whoami` — returns `{name, isAdmin}` for authenticated key
- `GET /config` — per-user config
- `POST /config` — update user config (blocked for admin)
- `GET /history` — conversation history for current user
- `POST /reset` — clear history, force-close active WebSocket (code 4002), delete resumption handle
- `GET/POST/DELETE /admin/keys` — user management, admin auth only
- `WS /ws?key=<key>` — WebSocket audio relay to Gemini Live

**WebSocket flow (`/ws`):**
1. Auth via `?key=` query param
2. Load user config (personality, voice) and session history
3. Build system prompt: base DM personality + continuation context (if history exists) or new game intro
4. Open Gemini Live session with `LiveConnectConfig` (audio modality, voice, transcription enabled)
5. Two async tasks: `esp_to_gemini` (forward binary PCM + handle ping/end_audio) and `gemini_to_esp` (forward audio chunks + transcriptions)
6. Transcriptions accumulated into complete turns, saved to disk on `turn_complete`
7. On disconnect, flush pending text and save session (unless reset flag set)

**Config fields:** `name`, `personality`, `voice` (Gemini voice name like `Algenib`, `Charon`, `Kore`, `Puck`).

**Defaults:** name=`SoloRolls DM`, voice=`Algenib`, personality loaded from `DM_PERSONALITY` env var (elaborate DM persona with Rollless Roleplay rules).

## Web UI (`worker/public/`)

Single-page app: login screen → chat screen with mic button, settings modal, admin panel.

**Audio:** Browser captures 16kHz mono PCM via `ScriptProcessor`, sends as binary WebSocket frames. Receives 24kHz mono PCM, plays via `AudioContext` buffer scheduling.

**WebSocket messages handled:** `transcript` (player speech), `response` (DM text), `interrupted` (stop playback), `pong` (heartbeat), `error`.

**Features:** real-time voice chat, conversation history display, game transcript download, new game (reset), per-user settings (name, personality, voice), admin user management.

## Firmware (`firmware/src/main.cpp`)

Hardware: AI-Lite = ESP32-S3, ES8311 codec (I2C + I2S), ST7735 LCD, 8MB PSRAM, two buttons.
Pin definitions in `firmware/src/pins.h`.

Key libs: `arduino-audio-tools`, `arduino-audio-driver`, `Arduino_GFX_Library`, `WebServer`, `DNSServer`, `ArduinoJson`, `links2004/WebSockets`.

**NVS (Preferences):** namespace `"ailite"` stores: `wifiSsid`, `wifiPass`, `workerUrl`, `apiKey`, `assistantName`, `ip`/`gw`/`sn`/`dns` (cached DHCP).

**Captive portal:** `WebServer`+`DNSServer` on AP `"SoloRolls-DM"`. HTML from `portal.h`. POST `/save` receives JSON config, writes NVS, restarts. Timeout 300s.

**Button mapping:** A (GPIO 1, RTC-capable) = wake + start session; B (GPIO 42) = config portal (hold at boot); Hold A + tap B = open settings menu.

**Settings menu:** On-device menu with 6 items — Volume, Mic Gain, Sleep Timer, Display (brightness), WiFi (network management), About (device info). Navigation: B = scroll, A = select, A+B held = back/exit. All settings saved to NVS on menu exit. Slider-style UI for Volume/Mic/Display; list-style for Sleep/WiFi. Defaults: volume 80%, mic 80%, sleep 2 min, brightness 100%.

**Boot flow:**
1. B held → `openPortal()` → `goSleep()`
2. Missing creds → show message → `openPortal()` → `goSleep()`
3. Connect WiFi → connect WebSocket → start face animation → wait for A press
4. A pressed → `startMicStream()` → continuous streaming until idle timeout

**Continuous mic streaming:** `micTask` on Core 1 reads I2S (24kHz stereo), downsamples to 16kHz mono, sends via WebSocket binary frames. Gemini's VAD handles turn-taking — no explicit "done" signal from device.

**Audio playback:** `playTask` on Core 1 reads from ring buffer (96KB PSRAM), upsamples 24kHz mono → 48kHz stereo for ES8311 I2S output. Prebuffer 6KB before starting playback.

**Animated face:** Wizard face on ST7735 LCD with states: IDLE (blinks), LISTEN (eyes wide), THINK (eyes shift), SPEAK (mouth animates), SLEEP (eyes closed). Runs at ~10fps on Core 1.

**WebSocket events:** Binary → ring buffer for playback; JSON `transcript` → FACE_LISTEN; `response` → FACE_SPEAK; `interrupted` → stop playback.

**Sleep:** `esp_sleep_enable_ext0_wakeup(PIN_BTN_A, 0)`. `rtc_gpio_hold_en(PIN_PWR_CTL)` keeps power rail active. Idle timeout: 2 minutes.

**Downsample:** 24kHz stereo → 16kHz mono via 3:2 linear interpolation.

## Docker

`Dockerfile` copies `local/server.py` and `worker/public/`. Runs on port 8787 internally.

`docker-compose.yml` maps 8765:8787, mounts `users.json` and `sessions/` for persistence.

## Legacy / Unused

- `wrangler.toml` — references a Cloudflare Worker (`worker/worker.js`) that no longer exists. The project migrated from CF Workers to the local Python server.

## Deploy

```bash
docker-compose up -d
```

Or directly:

```bash
python local/server.py
```

Required env: `GOOGLE_API_KEY`. Optional: `API_SECRET`, `PORT`, `GEMINI_LIVE_MODEL`, `DM_PERSONALITY`, `DM_NEW_GAME_INTRO`, `DM_CONTINUATION`.
