# ailite-cf

Cloudflare Worker + ESP32 firmware for an AI voice assistant on the [AI-Lite](https://www.elecrow.com/ai-lite-esp32-s3-ai-development-board.html) device.

Hold the button, speak, get a spoken response. All AI runs on Cloudflare — no external API keys needed.

https://github.com/user-attachments/assets/4efa78af-b0c0-42c3-a23f-0ffadb4cd5f6

## Device buttons

| Button | Action |
|--------|--------|
| A (left) | Hold to record voice; releases to send |
| B (right) | Hold anytime to open config portal |

After the response finishes playing, the device sleeps automatically. Hold A to wake and start a new turn.

## First-time setup

**Flash the firmware** — open `https://YOUR_WORKER.workers.dev/flash` in Chrome or Edge, plug in, click Flash.

**Configure the device** — on first boot (or hold B anytime), the device creates a WiFi AP called **AI-Lite-Setup**. Connect to it, fill in:
- Your home WiFi network + password
- Worker URL (`https://YOUR_WORKER.workers.dev`)
- Your API key (from the web UI)

The device restarts and sleeps. Hold A to wake it.

## Worker setup

You need a [Cloudflare account](https://dash.cloudflare.com/sign-up) (free tier works).

```bash
npm install

# Create KV namespace for user storage
npx wrangler kv namespace create CONFIG
# Paste the printed ID into wrangler.toml → id = "..."

# Set your admin secret
npm run admin_key
npx wrangler secret put API_SECRET

npm run deploy
```

Open `https://YOUR_WORKER.workers.dev/` — log in with your `API_SECRET` to create user keys, or with a user key to chat and configure your voice.

## Build firmware from source

Requires [PlatformIO](https://platformio.org/).

```bash
cd firmware
pio run --target upload
pio device monitor
```

To reset all saved settings:

```bash
pio run --target erase
```

## AI models

All run on Cloudflare — no third-party accounts needed.

| Step | Model |
|------|-------|
| Speech → Text | `@cf/openai/whisper-large-v3-turbo` |
| Text → Response | `@cf/meta/llama-3.1-8b-instruct` |
| Response → Speech | `@cf/deepgram/aura-2-en` or `aura-2-es` |

Cloudflare's free tier (10,000 neurons/day) covers hundreds of turns per day.

## API / WebSocket docs

See [API.md](API.md) for endpoint reference and WebSocket protocol details.
