# Server Guide

The Python server (`local/server.py`) acts as a relay between clients (ESP32 device or web browser) and the Gemini Live API. It handles authentication, session persistence, and user management.

## Stack

- **FastAPI** + **Uvicorn** — async HTTP/WebSocket server
- **google-genai** — Google's Gemini API client
- **python-dotenv** — loads `.env` file

## Running

### Docker (recommended)

```bash
cp .env.example .env
# Set GOOGLE_API_KEY in .env
docker-compose up -d
```

Docker maps host port **8765** → container port **8787**. User data and sessions are persisted via volume mounts.

### Local

```bash
cd local
pip install -r requirements.txt
export GOOGLE_API_KEY=your-key-here
python server.py
```

Runs on port 8765 by default.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GOOGLE_API_KEY` | — | Google AI API key (required) |
| `API_SECRET` | random hex | Admin authentication key |
| `PORT` | `8765` | Server listen port |
| `GEMINI_LIVE_MODEL` | `gemini-3.1-flash-live-preview` | Gemini Live model name |

If `API_SECRET` is not set, the server falls back to `API_KEY` env var, then generates a random token (printed at startup).

## Authentication

- **Admin**: authenticates with the `API_SECRET` value
- **Users**: authenticate with keys created by the admin via `/admin/keys`
- HTTP endpoints use `Authorization: Bearer <key>` header
- WebSocket uses `?key=<key>` query parameter

User keys and configs are stored in `local/users.json`.

## Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/` | none | Serves web UI (static files from `worker/public/`) |
| `GET` | `/whoami` | user | Returns `{name, isAdmin}` |
| `GET` | `/config` | user | Get user's DM config |
| `POST` | `/config` | user | Update user config (blocked for admin key) |
| `GET` | `/history` | user | Get conversation history |
| `POST` | `/reset` | user | Clear history + force-close active WebSocket (code 4002) |
| `GET` | `/admin/keys` | admin | List all user keys and configs |
| `POST` | `/admin/keys` | admin | Create new user → returns `{key, name, ...}` |
| `DELETE` | `/admin/keys/:key` | admin | Delete a user |
| `WS` | `/ws?key=<key>` | query | WebSocket audio relay to Gemini Live |

## User Config

Each user has three configurable fields:

| Field | Default | Description |
|-------|---------|-------------|
| `name` | `SoloRolls DM` | Display name for the assistant |
| `personality` | Built-in DM persona | Full system prompt sent to Gemini |
| `voice` | `Charon` | Gemini voice name |

Available Gemini voices: `Charon`, `Kore`, `Puck`, `Fenrir`, `Aoede`, and others.

## Session Management

Sessions are stored as JSON files in `local/sessions/<user_key>.json`. Each file contains:

```json
{
  "last_active": 1700000000.0,
  "history": [
    {"role": "user", "text": "I open the door"},
    {"role": "assistant", "text": "The door creaks open..."}
  ]
}
```

- Sessions **expire after 1 hour** of inactivity (auto-deleted on next load)
- History is injected into the Gemini system prompt for continuity
- `POST /reset` deletes the session file and force-closes the active WebSocket

## WebSocket Flow (`/ws`)

1. Client connects with API key in query param
2. Server loads user config (personality, voice) and session history
3. Builds system prompt:
   - **New game**: base personality + new game intro prompt
   - **Continuing**: base personality + continuation directive + full conversation history
4. Opens Gemini Live session with `LiveConnectConfig`:
   - Audio response modality
   - Configured voice
   - Input + output audio transcription enabled
5. Two async tasks run concurrently:
   - **esp_to_gemini**: forwards binary PCM from client + handles `ping`/`end_audio` messages
   - **gemini_to_esp**: forwards audio chunks + transcription text back to client
6. Transcriptions are accumulated into complete turns and saved to disk on `turn_complete`
7. On disconnect, pending text is flushed and session is saved (unless reset flag is set)

### Client → Server Messages

| Frame Type | Content | Meaning |
|------------|---------|---------|
| Binary | Raw 16kHz mono PCM | Mic audio |
| Text | `{"type":"ping"}` | Keepalive (server responds with `pong`) |
| Text | `{"type":"end_audio"}` | Client stopped recording (browser only) |

### Server → Client Messages

| Frame Type | Content | Meaning |
|------------|---------|---------|
| Binary | Raw 24kHz mono PCM | Gemini's voice response (chunked at 1024 bytes) |
| Text | `{"type":"transcript","text":"..."}` | Player's speech transcribed |
| Text | `{"type":"response","text":"..."}` | DM's response text |
| Text | `{"type":"interrupted"}` | Gemini interrupted (player started talking) |
| Text | `{"type":"reconnecting","message":"..."}` | Server-side session rotation |
| Text | `{"type":"pong"}` | Keepalive response |
| Text | `{"type":"error","message":"..."}` | Error message |

### Close Codes

| Code | Meaning |
|------|---------|
| 4001 | Invalid API key |
| 4002 | Session reset by user (new game) |

## DM Personality

The default personality is an elaborate DM persona that includes:
- Rollless Roleplay system rules (Success Points, narrative combat)
- Audio performance directions (dramatic delivery with emotion tags)
- NPC guidelines and world-building instructions
- A character voice profile ("Alistair W. — The Master of Realms")

Users can override this entirely via the `personality` config field.

## Docker Details

**Dockerfile:**
- Base: `python:3.12-slim`
- Copies `local/server.py`, `local/models.py`, `worker/public/`
- Exposes port 8787

**docker-compose.yml:**
- Maps `8765:8787`
- Mounts `./local/users.json` and `./local/sessions/` for persistence
- Loads `.env` file
- Sets `PORT=8787` inside container

## File Structure

```
local/
├── server.py          # Main server (Gemini Live relay)
├── models.py          # Legacy: non-Live Gemini + Fish TTS (unused by server.py)
├── requirements.txt   # Python dependencies
├── users.json         # User keys and configs (created at runtime)
└── sessions/          # Per-user session history (created at runtime)
```

## Legacy Code

`local/models.py` contains `GeminiModel` and `FishTTS` classes from a previous architecture that used separate STT → LLM → TTS pipeline. It's not imported by the current server but may be useful for future non-streaming features.
