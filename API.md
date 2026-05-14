# API Reference

All requests authenticated via `Authorization: Bearer <key>`. Admin endpoints require the primary `API_SECRET`; all others accept any valid user key.

## Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/` | none | Web UI |
| `GET` | `/whoami` | user | `{"name":"...","isAdmin":bool}` |
| `GET` | `/config` | user | Get user config |
| `POST` | `/config` | user | Update user config |
| `POST` | `/chat` | user | Voice or text chat (see below) |
| `GET` | `/ws?key=<key>` | query param | WebSocket voice pipeline |
| `GET` | `/admin/keys` | admin | List all users |
| `POST` | `/admin/keys` | admin | Create user → `{"key":"..."}` |
| `PUT` | `/admin/keys/:key` | admin | Update user config |
| `DELETE` | `/admin/keys/:key` | admin | Delete user |

## `/chat` modes

| Request body | Response |
|---|---|
| Raw WAV audio | MP3 stream |
| Raw WAV + `Accept: application/json` | `{"input":"...","text":"...","audio":"<base64>"}` |
| `{"text":"..."}` | `{"text":"...","audio":"<base64>"}` |

## User config fields

| Field | Values | Default |
|-------|--------|---------|
| `name` | string | — |
| `personality` | string (system prompt) | — |
| `ttsModel` | `aura-2-en`, `aura-2-es` | `aura-2-en` |
| `ttsVoice` | voice name for chosen model | `luna` |

## Voice models

| `ttsModel` | Language | Voices | Default voice |
|------------|----------|--------|---------------|
| `aura-2-en` | English | 39 | `luna` |
| `aura-2-es` | Spanish | 10 | `aquila` |

Full voice lists are in `worker/public/index.js` → `VOICES`.

## WebSocket protocol (`/ws`)

Connect: `wss://YOUR_WORKER.workers.dev/ws?key=YOUR_KEY`

The connection stays open for multi-turn conversation.

**Device → Worker**

| Frame | Meaning |
|-------|---------|
| Binary | Raw audio chunk (streamed while recording) |
| `{"type":"done"}` | Button released — triggers STT → LLM → TTS |

**Worker → Device** (in order per turn)

| Frame | Meaning |
|-------|---------|
| `{"type":"transcript","text":"..."}` | What Whisper heard |
| `{"type":"response","text":"..."}` | LLM reply text |
| `{"type":"audio_start"}` | TTS stream starting |
| Binary | MP3 frames (feed directly to decoder) |
| `{"type":"audio_end"}` | Turn complete |
| `{"type":"error","message":"..."}` | Pipeline error |
