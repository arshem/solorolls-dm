# Web UI Guide

The web UI is a single-page app served by the Python server from `worker/public/`. It provides browser-based voice chat with the DM, independent of the ESP32 device.

## Files

```
worker/public/
├── index.html      # Page structure (login, chat, settings modal)
├── index.js        # All application logic
├── style.css       # Dark theme styling
├── solorolls.png   # Logo
└── favicon/        # PWA icons and manifest
```

## Features

- Real-time voice chat with the DM via WebSocket
- Live transcription display (both player speech and DM responses)
- Conversation history loaded on connect
- Download game transcript as text file
- Start new game (clears history, reconnects)
- Per-user settings (DM name, personality, voice)
- Admin panel for user management (create, edit, delete)
- PWA-capable (mobile web app manifest)

## Authentication

1. User enters their API key on the login screen
2. Key is stored in `localStorage`
3. All HTTP requests use `Authorization: Bearer <key>` header
4. WebSocket connects with `?key=<key>` query param
5. Invalid key → server closes WebSocket with code 4001 → redirects to login

## Audio

### Recording (browser → server)

- Captures mic at **16kHz mono** via `getUserMedia`
- Uses `ScriptProcessor` (4096 sample buffer) to convert float32 → int16 PCM
- Sends raw PCM as binary WebSocket frames continuously while mic is active
- Sends `{"type":"end_audio"}` when mic is stopped

### Playback (server → browser)

- Receives **24kHz mono PCM** as binary WebSocket frames
- Converts int16 → float32 and creates `AudioBuffer` objects
- Schedules playback via `AudioContext.createBufferSource()` with gapless timing
- `interrupted` message stops playback and resets the audio context

## WebSocket Connection

- Connects on login: `ws[s]://<host>/ws?key=<key>`
- Auto-reconnects with exponential backoff (1s → 2s → 4s → ... → 30s max)
- Heartbeat ping every **25 seconds** (`{"type":"ping"}`)
- Handles close codes:
  - **4001** — invalid key, redirect to login
  - **4002** — session reset (new game), auto-reconnects

## Message Handling

| Incoming Message | UI Action |
|-----------------|-----------|
| Binary (PCM) | Queue for audio playback |
| `transcript` | Append to current user message bubble |
| `response` | Append to current DM message bubble |
| `interrupted` | Finalize messages, stop audio playback |
| `pong` | (ignored, heartbeat ack) |
| `error` | Display error in chat |

Transcription fragments are accumulated into message bubbles — a new bubble starts when the speaker changes (user → DM or DM → user).

## Settings Modal

Accessible via the gear icon in the top bar:

- **Assistant Name** — display name for the DM
- **Personality** — full system prompt (textarea)
- **Voice** — Gemini voice name (e.g. `Charon`, `Kore`, `Puck`)
- **Save Config** — `POST /config` with the three fields

Admin users also see a user management section:
- Create new users (generates API key, shown once)
- View existing users (key prefix + name)
- Delete users

## Game Actions

| Button | Action |
|--------|--------|
| Mic (center) | Toggle recording — tap to start, tap again to stop |
| Save (bottom left) | Download full transcript as `solorolls-game.txt` |
| New Game (bottom right) | Confirm → `POST /reset` → reconnect WebSocket |

## Styling

Dark theme with CSS custom properties:

- Background: deep navy/black (`#0f0f1a`, `#1a1a2e`)
- Accent: indigo (`#5a67d8`)
- Chat bubbles: user (dark gray), DM (dark blue)
- Responsive: mobile-first with desktop adjustments at 600px
- Safe area support for notched devices
- Animations: message fade-in, mic pulse ring while recording

## Browser Compatibility

- Requires `getUserMedia` (mic access)
- Requires `AudioContext` (playback)
- Requires WebSocket support
- Uses `ScriptProcessor` (deprecated but widely supported; `AudioWorklet` would be the modern alternative)
- No build step — vanilla HTML/JS/CSS served directly
