# SoloRolls DM — Local Server

FastAPI server that relays audio between the ESP32 device (or web browser) and Google's Gemini Live API for real-time bidirectional voice conversation. Acts as a D&D Dungeon Master using the Rollless Roleplay system.

## Stack

- **AI**: Google Gemini Live API (`gemini-3.1-flash-live-preview`) — handles STT, LLM, and TTS natively in a single streaming session
- **Server**: FastAPI + uvicorn
- **Audio format**: 16kHz mono PCM in (from device), 24kHz mono PCM out (to device)

## Quick Start

```bash
cd local
pip install -r requirements.txt

# Required: Google AI API key
export GOOGLE_API_KEY=your-google-api-key

# Optional: Admin secret for user management (random if not set)
export API_SECRET=your-secret-here

python server.py
```

The server runs on port 8765 by default (set `PORT` env var to change).

## How It Works

1. **ESP32 connects** via WebSocket — continuous mic streaming begins
2. **Gemini's VAD** detects when the player is speaking (no button-hold needed for turn detection)
3. **Gemini responds** in real-time with voice audio — the DM narrates your adventure
4. **Audio streams back** as 24kHz mono PCM to the device speaker
5. **Transcriptions** are sent as JSON text frames for the display/UI

No separate STT → LLM → TTS pipeline. Gemini Live handles the full conversation natively with sub-second latency.

## Rollless Roleplay System

- No dice rolls — players have **Success Points**
- Spend points to succeed at tasks (difficulty 1–5)
- Earn points through good roleplay, story advancement, creative solutions
- Combat is narrative-based, no hit points
- Collaborative world-building with the player

On first session, the DM asks what kind of campaign you want to play.

## Session Persistence

The server maintains per-user conversation history on disk (`sessions/` directory). History is injected into the Gemini system prompt on reconnect so the DM remembers your campaign. Sessions auto-expire after 1 hour of inactivity.

## Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/whoami` | Bearer | Returns `{name, isAdmin}` |
| `GET` | `/config` | Bearer | Get user config |
| `POST` | `/config` | Bearer | Update user config |
| `GET` | `/history` | Bearer | View conversation log |
| `POST` | `/reset` | Bearer | Clear history, start new adventure |
| `GET` | `/admin/keys` | Admin | List all users |
| `POST` | `/admin/keys` | Admin | Create user |
| `DELETE` | `/admin/keys/:key` | Admin | Delete user |
| `WS` | `/ws?key=<key>` | Query param | WebSocket audio relay |

## WebSocket Protocol (`/ws`)

**Device → Server:**
- Binary frames: raw 16kHz mono PCM audio chunks
- `{"type":"end_audio"}`: signal end of audio stream (browser mic stop)
- `{"type":"ping"}`: keepalive

**Server → Device:**
- Binary frames: raw 24kHz mono PCM audio chunks (from Gemini)
- `{"type":"transcript","text":"..."}`: player's transcribed speech
- `{"type":"response","text":"..."}`: DM's response text
- `{"type":"interrupted"}`: Gemini interrupted (player started talking)
- `{"type":"pong"}`: keepalive response
- `{"type":"error","message":"..."}`: error

## Docker

```bash
docker-compose up -d
```

The Dockerfile copies `local/server.py`, `local/models.py`, and `worker/public/` (web UI). Volumes mount `users.json` and `sessions/` for persistence.

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GOOGLE_API_KEY` | — | Google AI API key (required) |
| `API_SECRET` | random | Admin key for user management |
| `PORT` | `8765` | Server port |
| `GEMINI_LIVE_MODEL` | `gemini-3.1-flash-live-preview` | Gemini Live model name |

### Per-User Config

| Field | Description | Default |
|-------|-------------|---------|
| `name` | DM display name | `SoloRolls DM` |
| `personality` | System prompt (DM persona) | Built-in Alistair W. persona |
| `voice` | Gemini voice name | `Charon` |

## User Management

Users are stored in `users.json`. The admin key (from `API_SECRET` env var) can create/delete users via the API or web UI. Each user gets their own session history and config.
