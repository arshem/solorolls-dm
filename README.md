# SoloRolls DM

Solo D&D Dungeon Master powered by Google Gemini Live API, running on an [AiPi Lite](https://aipi.com/products/aipi-lite) (ESP32-S3) device with a local Python server.

Tap the button and start your adventure — the DM narrates in real-time. Gemini handles speech recognition, storytelling, and voice synthesis natively (no separate STT/LLM/TTS pipeline).

## Architecture

```
ESP32 (AiPi Lite) ←── WebSocket (16kHz PCM in, 24kHz PCM out) ──→ Python Server ←──→ Gemini Live API
                                                                       ↕
                                                                  Web UI (browser)
```

- **ESP32 firmware** — streams mic audio continuously; Gemini's built-in VAD detects when you're speaking
- **Python server** — relays audio between the device and Gemini Live, manages sessions and users
- **Web UI** — browser-based voice chat, settings, and admin panel (works independently of the device)

## Game System

Uses the **Rollless Roleplay** system — no dice, no hit points. Players spend Success Points to overcome challenges, earn them through good roleplay and creative problem-solving. Combat is narrative-based. The DM collaborates with you on world-building.

## Device Buttons

| Button | Action |
|--------|--------|
| A (left) | Press to start session; device streams mic continuously after |
| B (right) | Hold at boot to open config portal; hold 1s during session for portal |
| Hold A + tap B | Open settings menu (during a session) |

After idle timeout (default 2 min, configurable), the device deep-sleeps. Press A to wake.

## Settings Menu

Hold A + tap B during a session to open the on-device settings menu. Navigate with B (scroll), A (select), and hold A+B (back/exit). All settings persist to flash on menu exit.

| Menu Item | Description |
|-----------|-------------|
| Volume | Speaker volume: 20%, 40%, 60%, 80%, 100% (default 80%) |
| Mic Gain | Microphone sensitivity: 20%, 40%, 60%, 80%, 100% (default 80%) |
| Sleep Timer | Idle timeout before deep-sleep: 2 min, 5 min, 10 min, Never (default 2 min) |
| Display | LCD brightness: ~10%, 25%, 50%, 75%, 100% (default 100%) |
| WiFi | View saved networks, switch active, or add new (opens captive portal) |
| About | Firmware version, WiFi SSID, RSSI, IP, WebSocket status, battery voltage |

## Quick Start

### Server (Docker)

```bash
cp .env.example .env
# Edit .env and set GOOGLE_API_KEY
docker-compose up -d
```

The server runs on host port 8765 (mapped to 8787 inside the container).

### Server (Local)

```bash
cd local
pip install -r requirements.txt
export GOOGLE_API_KEY=your-key-here
python server.py
```

### First-time Device Setup

1. Flash the firmware (see [Firmware](#firmware) below)
2. Hold B on boot — the device creates a WiFi AP called **SoloRolls-DM**
3. Connect to it and fill in:
   - Your WiFi network + password
   - Server URL (e.g. `https://yourdomain.com` or `http://192.168.1.x:8765`)
   - API key (create one from the web UI admin panel)
4. Device restarts and sleeps. Press A to wake and begin your adventure.

## Firmware

Targets ESP32-S3 (AiPi Lite) with ES8311 codec, ST7735 128x128 LCD, 8MB PSRAM.

### Compile

```bash
cd firmware
pio run
```

This produces `firmware/.pio/build/ailite/firmware.factory.bin`.

### Flash

```bash
esptool --chip esp32s3 write_flash 0x0 firmware/.pio/build/ailite/firmware.factory.bin
```

### Erase all saved settings (NVS)

```bash
esptool --chip esp32s3 erase_flash
```

## Web UI

Open your server URL in a browser. Log in with your API key to:
- Voice chat with the DM (tap mic to talk)
- View real-time transcriptions of both player and DM speech
- Download game transcripts
- Start a new game (resets conversation history)
- Configure DM personality and voice (Settings)
- Manage users (admin only)

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GOOGLE_API_KEY` | — | Google AI API key (required) |
| `API_SECRET` | random | Admin key for user management |
| `PORT` | `8765` | Server port |
| `GEMINI_LIVE_MODEL` | `gemini-3.1-flash-live-preview` | Gemini Live model |

### Per-User Config (via `/config` endpoint or Settings UI)

| Field | Description |
|-------|-------------|
| `name` | Assistant/DM display name |
| `personality` | System prompt for the DM |
| `voice` | Gemini voice name (`Charon`, `Kore`, `Puck`, `Fenrir`, `Aoede`, etc.) |

## API

See [API.md](API.md) for endpoint reference and WebSocket protocol details.

## Further Documentation

See the [docs/](docs/) folder for detailed guides:
- [Firmware Guide](docs/firmware.md) — hardware details, boot flow, audio pipeline, face animation
- [Server Guide](docs/server.md) — session management, Gemini integration, deployment
- [Web UI Guide](docs/webui.md) — browser audio, WebSocket handling, features

## Deploy

```bash
docker-compose up -d
```

Or run directly:

```bash
python local/server.py
```
