# ailite-cf

Cloudflare Worker + ESP32 firmware for an AI voice assistant on the [AI-Lite](https://www.elecrow.com/ai-lite-esp32-s3-ai-development-board.html) device.

Press the button, speak, get a spoken response. All AI runs on Cloudflare — no external services needed.

https://github.com/user-attachments/assets/4efa78af-b0c0-42c3-a23f-0ffadb4cd5f6


## How it works

1. [flash your aipilite](https://ailite.konsumer.workers.dev/flash)
2. First boot (or hold button A on boot): connect to **AI-Lite-Setup** WiFi AP, enter your WiFi credentials, Worker URL, and API key in the captive portal
3. Device sleeps — hold **button B** to wake
4. Hold button B and speak; the device records your voice over an open WebSocket
5. Release button B — audio is sent to the worker
6. Worker runs: Whisper STT → Llama LLM → Deepgram Aura-2 TTS
7. Device plays the streamed MP3 response through the speaker
8. Device sleeps after 10 seconds

## Worker setup

You need a [Cloudflare account](https://dash.cloudflare.com/sign-up) (free tier works).

```bash
npm install

# Create the KV namespace for personality storage
npx wrangler kv namespace create CONFIG
# Copy the printed ID into wrangler.toml → id = "..."

# Set your API secret (used to authenticate the device)
npm run admin_key
npx wrangler secret put API_SECRET

# Deploy
npm run deploy
```

### Web UI

After deploying, open `https://YOUR_WORKER.workers.dev/` in a browser. Log in with your primary `API_SECRET` to manage users, or with a user key to chat and configure.

### User management

The primary `API_SECRET` is admin-only. Create user keys through the web UI admin panel, or via API:

```bash
# Create a user
curl -X POST https://YOUR_WORKER.workers.dev/admin/keys \
  -H "Authorization: Bearer your-primary-secret" \
  -H "Content-Type: application/json" \
  -d '{"name": "Alice"}'
# Returns: {"key": "<key to share with Alice>", ...}

# List users
curl https://YOUR_WORKER.workers.dev/admin/keys \
  -H "Authorization: Bearer your-primary-secret"

# Update a user
curl -X PUT https://YOUR_WORKER.workers.dev/admin/keys/<key> \
  -H "Authorization: Bearer your-primary-secret" \
  -H "Content-Type: application/json" \
  -d '{"name": "Alice", "ttsModel": "aura-2-en", "ttsVoice": "luna"}'

# Delete a user
curl -X DELETE https://YOUR_WORKER.workers.dev/admin/keys/<key> \
  -H "Authorization: Bearer your-primary-secret"
```

Each user gets their own key and can configure their own personality and voice.

### Endpoints

| Method | Path | Auth | Body | Response |
|--------|------|------|------|----------|
| `GET` | `/` | none | — | Web UI |
| `GET` | `/whoami` | user | — | `{"name":"...","isAdmin":bool}` |
| `POST` | `/chat` | user | raw WAV audio | MP3 stream |
| `POST` | `/chat` | user | `{"text":"..."}` | `{"text":"...","audio":"<base64>"}` |
| `GET` | `/config` | user | — | user config JSON |
| `POST` | `/config` | user | config fields | `OK` |
| `GET` | `/admin/keys` | primary | — | list of users |
| `POST` | `/admin/keys` | primary | `{"name":"..."}` | `{"key":"..."}` |
| `PUT` | `/admin/keys/:key` | primary | config fields | `OK` |
| `DELETE` | `/admin/keys/:key` | primary | — | `OK` |
| `GET` | `/ws?key=<key>` | query param | WebSocket upgrade | streaming voice pipeline |

### WebSocket voice pipeline

The `/ws` endpoint is the real-time path for devices. Connect with your API key as a query param:

```
ws://YOUR_WORKER.workers.dev/ws?key=YOUR_USER_KEY
```

**Device → Worker** (binary frames = audio chunks; text frames = JSON control):
- Stream audio binary frames as the user speaks
- Send `{"type":"done"}` when the button is released

**Worker → Device** (in order):
- `{"type":"transcript","text":"..."}` — what Whisper heard
- `{"type":"response","text":"..."}` — LLM reply text
- `{"type":"audio_start"}` — TTS about to stream
- Binary MP3 frames (pipe directly to decoder as they arrive)
- `{"type":"audio_end"}` — done, ready for next turn
- `{"type":"error","message":"..."}` — if anything fails

The connection stays open for multi-turn conversation. No reconnect needed between questions.

### Voice models

| `ttsModel` | Language | Voices | Streaming |
|------------|----------|--------|-----------|
| `aura-2-en` | English | 39 | yes |
| `aura-2-es` | Spanish | 10 | yes |

Aura-2 streams audio as it generates — the device can start playing before the full response is ready. The web UI lets you pick English or Español and choose a voice from the correct list for that language.

### Local development

```bash
npm run dev
```

Note: AI bindings require `--remote`. The `dev` script already includes it.

## Firmware setup

### Easy install (no toolchain needed)

Open **`https://YOUR_WORKER.workers.dev/flash`** in Chrome or Edge, plug in the device, click Flash. Uses the latest release binary.

### Build from source

Requirements: [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

No hardcoded config needed. Flash as-is:

```bash
cd firmware
pio run --target upload
pio device monitor   # to see serial output
```

### First boot setup

On first boot the device appears as a WiFi AP named **AI-Lite-Setup**. Connect to it and a captive portal opens. Enter:
- Your home WiFi credentials
- Worker URL (`https://YOUR_WORKER.workers.dev`)
- Your user API key (created in the web UI)

Settings are saved to flash. The device then sleeps until button B is held.

To reconfigure at any time, hold **button A** on boot — the portal reopens.

To reset everything, erase flash:

```bash
pio run --target erase
```

### Button reference

| Button | Action |
|--------|--------|
| A (left) | Hold to wake from sleep / hold to record voice |
| B (right) | Hold on boot (or hold A then also hold B) to open config portal |

After playback finishes, the device sleeps automatically after 10 seconds.

Note: only GPIO 0–21 can wake the ESP32-S3 from deep sleep (RTC GPIO constraint). Button A = GPIO 1 is RTC-capable; Button B = GPIO 42 is not, so A is the wake source.

## AI models used

All run on Cloudflare's network — no third-party API keys needed beyond your CF account.

| Step | Model |
|------|-------|
| Speech → Text | `@cf/openai/whisper-large-v3-turbo` |
| Text → Response | `@cf/meta/llama-3.1-8b-instruct` |
| Response → Speech | `@cf/deepgram/aura-2-en` or `@cf/deepgram/aura-2-es` |

## Costs

Cloudflare Workers AI has a free tier (10,000 neurons/day). Each conversation turn uses roughly:
- Whisper: ~5–15 neurons depending on audio length
- Llama 3.1 8B: ~10–30 neurons depending on response length
- Aura-2: ~5–10 neurons

In practice, the free tier supports hundreds of conversations per day.

## Project structure

```
ailite-cf/
├── worker/
│   ├── worker.js         ← Cloudflare Worker
│   └── public/           ← Web UI (static assets)
├── firmware/
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp      ← ESP32 firmware
│       └── pins.h        ← GPIO definitions
├── test/
│   ├── stream.test.mjs   ← WebSocket pipeline tests
│   └── test.wav          ← test audio
├── wrangler.toml
└── package.json
```
