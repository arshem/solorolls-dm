# API Reference

All HTTP requests authenticated via `Authorization: Bearer <key>`. Admin endpoints require the `API_SECRET`; all others accept any valid user key. WebSocket auth uses the `?key=` query parameter.

## Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/` | none | Web UI (static files) |
| `GET` | `/whoami` | user | `{"name":"...","isAdmin":bool}` |
| `GET` | `/config` | user | Get user config |
| `POST` | `/config` | user | Update user config (blocked for admin) |
| `GET` | `/history` | user | Get conversation history |
| `POST` | `/reset` | user | Clear history, force-close active session |
| `GET` | `/admin/keys` | admin | List all users |
| `POST` | `/admin/keys` | admin | Create user → `{"key":"...","name":"..."}` |
| `DELETE` | `/admin/keys/:key` | admin | Delete user |
| `WS` | `/ws?key=<key>` | query param | WebSocket audio relay to Gemini Live |

## User Config Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | `SoloRolls DM` | Display name for the assistant |
| `personality` | string | Built-in DM persona | System prompt sent to Gemini |
| `voice` | string | `Charon` | Gemini voice name |

Available voices include: `Charon`, `Kore`, `Puck`, `Fenrir`, `Aoede`, and others supported by Gemini Live.

## WebSocket Protocol (`/ws`)

Connect: `wss://YOUR_SERVER/ws?key=YOUR_KEY` (or `ws://` for local HTTP)

The connection stays open for the duration of the session. Audio flows bidirectionally in real-time — Gemini's built-in VAD handles turn-taking.

### Device/Browser → Server

| Frame | Meaning |
|-------|---------|
| Binary | Raw 16kHz mono PCM audio chunks (continuous mic stream) |
| `{"type":"end_audio"}` | Mic stopped (browser only — signals end of audio input) |
| `{"type":"ping"}` | Keepalive |

### Server → Device/Browser

| Frame | Meaning |
|-------|---------|
| Binary | Raw 24kHz mono PCM audio chunks (Gemini's voice response) |
| `{"type":"transcript","text":"..."}` | Player's speech transcribed by Gemini |
| `{"type":"response","text":"..."}` | DM's response text (output transcription) |
| `{"type":"interrupted"}` | Gemini was interrupted (player started talking) |
| `{"type":"reconnecting","message":"..."}` | Server-side session rotation |
| `{"type":"pong"}` | Keepalive response |
| `{"type":"error","message":"..."}` | Error |

### Connection Lifecycle

1. Client connects with API key in query param
2. Server opens a Gemini Live session with the user's personality/voice config
3. If conversation history exists, it's injected into the system prompt for continuity
4. Audio flows bidirectionally — no explicit "done" signal needed (Gemini VAD)
5. Transcriptions stream as text frames alongside audio
6. Session persists on disk; reconnecting resumes the campaign
7. `POST /reset` force-closes the WebSocket (code 4002) and clears history

### Close Codes

| Code | Meaning |
|------|---------|
| 4001 | Invalid API key |
| 4002 | Session reset by user (new game) |
