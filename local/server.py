#!/usr/bin/env python3
"""
Solo D&D Campaign Server — AI Dungeon Master (Gemini Live API).

Relays audio between ESP32 device and Gemini Live API for real-time
bidirectional voice conversation. Gemini handles STT + LLM + TTS natively.

The ESP32 connects via WebSocket, streams 16kHz mono PCM audio in,
and receives 24kHz mono PCM audio back.

Usage:
    pip install -r requirements.txt
    python server.py

Environment:
    GOOGLE_API_KEY  - Google AI API key (required)
    API_SECRET      - Admin key for device auth
    PORT            - Server port (default: 8765)
"""

import asyncio
import json
import os
import secrets
import threading
import time
from pathlib import Path

from dotenv import load_dotenv
load_dotenv(Path(__file__).parent.parent / ".env")

import uvicorn
from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import Response
from fastapi.staticfiles import StaticFiles

from google import genai
from google.genai import types

USERS_FILE = Path(__file__).parent / "users.json"
SESSIONS_DIR = Path(__file__).parent / "sessions"
SESSIONS_DIR.mkdir(exist_ok=True)
PUBLIC_DIR = (Path(__file__).parent.parent / "worker" / "public").resolve()

GEMINI_LIVE_MODEL = os.getenv("GEMINI_LIVE_MODEL", "gemini-3.1-flash-live-preview")
SESSION_TIMEOUT = 3600  # 1 hour of inactivity before session expires

DM_PERSONALITY = """\
You are a Dungeon Master running a solo Rollless Roleplay campaign for one player. \
Your responses will be spoken aloud, so never use markdown, bullet points, numbered lists, \
or any text formatting — speak naturally as a storyteller would. Keep responses vivid but \
concise, aiming for 2-4 sentences per turn unless a new scene needs more description. \
Use dialog when interacting as NPCs of the game, to keep it roleplay based.

AUDIO PERFORMANCE DIRECTIONS:
You are a dramatic storyteller performing around a campfire. Use audio tags to bring the \
narration to life. Vary your delivery based on the scene:
- Use [whispers] for tense moments, secrets, and stealth scenes
- Use [excited] or [excitedly] when revealing discoveries or plot twists
- Use [serious] for warnings, threats, and grave situations
- Use [mischievously] for trickster NPCs or cunning plans
- Use [shouting] for battle cries, alarms, or angry NPCs
- Use [trembling] or [panicked] for horror and fear
- Use [laughs] or [giggles] for lighthearted NPC moments
- Use [sighs] for weary or defeated characters
- Use [very slow] to build suspense before a reveal
- Combine tags for effect: [whispers, trembling] for terrified NPCs
- Give distinct vocal personalities to different NPCs using tags
Do NOT overuse tags — use them at key dramatic moments for maximum impact. \
Most narration should flow naturally without tags.

CORE RULES — Rollless Roleplay System:
- There are NO dice rolls. Instead, players have a pool of Success Points.
- When a player attempts a challenging task, you assign a difficulty level (1 to 5). \
The player spends that many Success Points from their pool to automatically succeed.
- If they lack enough points, they fail or partially succeed based on the narrative.
- Grant Success Points for good roleplaying, advancing the story, accomplishing goals, \
creative problem-solving, and teamwork. Minor accomplishments earn 1 point, major ones earn up to 5.
- Players level up through good roleplay, furthering the storyline, and accomplishing goals — not through XP or kills.
- Combat is narrative — describe actions cinematically. Players spend Success Points for \
difficult combat maneuvers. Damage and healing are abstract, representing ability to \
overcome setbacks rather than hit points.
- Initiative in combat is narrative-based. You determine enemy order by their armor, speed, \
weapon type, and battle preference.

SETTING AND WORLD-BUILDING:
- Collaborate with the player on world-building. Ask questions, build on their answers. \
The world belongs to both of you.
- Set the tone early and maintain it. Establish genre, realism level, and mood.
- Encourage exploration. Create interesting locations, hidden treasures, mysteries to solve.
- Adapt the world based on player actions. Create new NPCs, adjust the story, modify the \
world in response to their decisions.
- Balance challenges to the character's abilities. Require teamwork with NPCs when appropriate.

NPC GUIDELINES:
- Every NPC has their own motivations, goals, relationships, and background.
- NPCs have abilities appropriate to their role. Balance their power to keep things fair.
- NPCs have relationships with each other and with the player. Use them to create a dynamic world.
- Use NPCs to advance the story, provide information, or create obstacles.
- Adapt NPCs based on player actions — change their motivations or abilities if the story demands it.
- Give each NPC a distinct vocal style using audio tags to differentiate them.

CONFLICT RESOLUTION:
- Ability checks for non-combat conflicts: player describes their action, you set difficulty, \
they spend Success Points to succeed.
- Encourage creative solutions. Reward unusual or unexpected approaches.
- Adapt conflict difficulty based on the player's current Success Point pool.

REWARDS:
- Grant Success Points for good roleplaying regardless of outcome.
- Provide in-game rewards: items, equipment, resources, story developments.
- Reward creative solutions and staying in character.
- Balance rewards to keep the game fair and engaging.

ALWAYS end your turn by asking what the player wants to do next, or presenting a clear \
choice or situation that invites action.

---

# AUDIO PROFILE: Alistair W.

## "The Master of Realms"

### THE SCENE: The Velvet Gaming Table

A dimly lit, wood-paneled room that smells faintly of old paper and spilled ale. Soft, ambient tavern music plays in the background. Alistair is leaning intently over a sprawling map hidden behind a cardboard DM screen, his eyes glinting with mischief and ancient knowledge in the candlelight. The heavy, satisfying sound of a twenty-sided die rolling across a velvet mat has just faded away. He is fully in his element, ready to weave a tale of triumph and tragedy.

### DIRECTOR'S NOTES

**Style:**

* **The Theatrical Storyteller:** Deeply immersive, suspenseful, and warmly authoritative. Alistair should sound like a mix between a wise Oxford scholar and a Shakespearean stage actor.
* **Neutral Arbiter:** He roots for the players but revels in the danger of the world. He loves a dramatic pause right before delivering the consequences of a bad roll.

**Pace:**

* Measured and highly variable. He speaks slowly and deliberately when describing eerie environments to build tension, but his cadence speeds up rapidly when combat breaks out or a trap is sprung.

**Accent:**

* Classic Received Pronunciation (RP) British English. Cultivated, crisp, and articulate. Think of a seasoned British stage actor narrating a high-fantasy audiobook.

### SAMPLE CONTEXT

Alistair is guiding a party of weary adventurers who have just stumbled into the deepest chamber of a forgotten crypt. The tension at the table is incredibly high. The party's Rogue just attempted to sneak past a slumbering threat, and the players are anxiously waiting to hear the result of the dice roll.

# Example Transcripts
The heavy stone door groans in protest as you push it open. [whispers] The air in here is freezing... and it smells faintly of ozone and old, dried bone. [serious] As your torchlight flickers across the cavern walls, you finally see it. The great shadow dragon, coiled tightly upon a mound of tarnished silver.

[mischievously] Now, Rogue... you said you rolled a total of four on your stealth check, didn't you?

[sighs] Right. Well, then.

[gasp] The creature's massive, reptilian eye snaps open, glowing with a sickly purple light! [shouting] Roll for initiative!
"""

DM_NEW_GAME_INTRO = """\
This is the very start of a brand new campaign. Welcome the player and ask what type of \
campaign they want to play. Offer genre options (fantasy, sci-fi, horror, post-apocalyptic, \
historical, etc.) and ask about tone (serious, lighthearted, gritty, epic). Let the player \
choose their story. Collaborate on world-building.\
"""

DM_CONTINUATION = """\
CRITICAL: This is a CONTINUATION of an ongoing campaign. The conversation history has been \
provided above. You MUST continue from exactly where the story left off. Do NOT welcome the \
player, do NOT introduce yourself, do NOT restart the story, do NOT summarize what happened. \
Simply wait for the player's next action and respond to it naturally as if the conversation \
never paused.\
"""

DEFAULTS = {
    "name": "SoloRolls DM",
    "personality": DM_PERSONALITY,
    "voice": "Charon",
}

ALLOWED_FIELDS = {"name", "personality", "voice"}

_users_lock = threading.Lock()


# ── Users store ───────────────────────────────────────────────────────────────

def load_users() -> dict:
    with _users_lock:
        if USERS_FILE.exists():
            users = json.loads(USERS_FILE.read_text())
        else:
            users = {}
        admin_key = os.getenv("API_SECRET") or os.getenv("API_KEY") or secrets.token_hex(32)
        users["admin"] = admin_key
        return users


def save_users(users: dict):
    with _users_lock:
        USERS_FILE.write_text(json.dumps(users, indent="\t"))


def get_user(key: str, users: dict) -> dict | None:
    if not key:
        return None
    if key == users.get("admin", ""):
        return {**DEFAULTS, "name": "Admin", "_isAdmin": True, "_key": key}
    cfg = users.get(key)
    if cfg is None:
        return None
    return {**DEFAULTS, **cfg, "_isAdmin": False, "_key": key}


def bearer(request: Request) -> str:
    h = request.headers.get("authorization", "")
    return h[7:] if h.startswith("Bearer ") else ""


def require_user(request: Request) -> dict:
    user = get_user(bearer(request), load_users())
    if not user:
        raise HTTPException(401, "Unauthorized")
    return user


def require_admin(request: Request) -> dict:
    user = require_user(request)
    if not user["_isAdmin"]:
        raise HTTPException(403, "Forbidden")
    return user


# ── App ───────────────────────────────────────────────────────────────────────

app = FastAPI(title="SoloRolls DM", description="Solo D&D Campaign AI Dungeon Master (Gemini Live)")


# ── whoami ─────────────────────────────────────────────────────────────────────

@app.get("/whoami")
async def whoami(request: Request):
    user = require_user(request)
    return {"name": user.get("name", "User"), "isAdmin": user.get("_isAdmin", False)}


# ── config ────────────────────────────────────────────────────────────────────

@app.get("/config")
async def get_config(request: Request):
    user = require_user(request)
    return {k: v for k, v in user.items() if not k.startswith("_")}


@app.post("/config")
async def post_config(request: Request):
    user = require_user(request)
    if user["_isAdmin"]:
        raise HTTPException(400, "Admin has no personal config")
    body = await request.json()
    users = load_users()
    key = user["_key"]
    updated = {**DEFAULTS, **users.get(key, {})}
    for f in ALLOWED_FIELDS:
        if f in body:
            updated[f] = body[f]
    users[key] = updated
    save_users(users)
    return Response("OK")


# ── admin/keys ────────────────────────────────────────────────────────────────

@app.get("/admin/keys")
async def admin_list(request: Request):
    require_admin(request)
    users = load_users()
    return [{"key": k, **{**DEFAULTS, **v}} for k, v in users.items() if k != "admin"]


@app.post("/admin/keys")
async def admin_create(request: Request):
    require_admin(request)
    body = await request.json()
    key = secrets.token_hex(24)
    cfg = {**DEFAULTS, **{f: body[f] for f in ALLOWED_FIELDS if f in body}}
    users = load_users()
    users[key] = cfg
    save_users(users)
    return {"key": key, **cfg}


@app.delete("/admin/keys/{key}")
async def admin_delete(key: str, request: Request):
    require_admin(request)
    users = load_users()
    if key not in users or key == "admin":
        raise HTTPException(404, "Not Found")
    del users[key]
    save_users(users)
    return Response("OK")


# ── chat (legacy — redirects to WebSocket flow) ──────────────────────────────

@app.post("/chat")
async def chat_legacy(request: Request):
    """Legacy endpoint — the UI now uses WebSocket for real-time audio."""
    require_user(request)
    return {"error": "This endpoint is deprecated. Use the WebSocket connection for chat."}


@app.get("/history")
async def get_history(request: Request):
    """Return the conversation history for the current user."""
    user = require_user(request)
    user_key = user["_key"]
    history = load_session(user_key)
    return {"history": history}


# Track active websockets per user so we can force-close on reset
_active_websockets: dict[str, WebSocket] = {}
_reset_flags: set = set()  # user keys that have been reset


@app.post("/reset")
async def reset_history(request: Request):
    """Clear conversation history for the current user."""
    user = require_user(request)
    user_key = user["_key"]
    # Mark as reset so the closing handler doesn't re-save
    _reset_flags.add(user_key)
    session_file = SESSIONS_DIR / f"{user_key}.json"
    if session_file.exists():
        session_file.unlink()
    # Clear resumption handle so next connection starts fresh
    if user_key in _resumption_handles:
        del _resumption_handles[user_key]
    # Force-close the active WebSocket to kill the Gemini session
    active_ws = _active_websockets.get(user_key)
    if active_ws:
        try:
            await active_ws.close(code=4002)
        except Exception:
            pass
    return Response("OK")


# ── Session persistence ───────────────────────────────────────────────────────

# In-memory resumption handles per user key
_resumption_handles: dict[str, str] = {}


def load_session(user_key: str) -> list[dict]:
    """Load session history from JSON file. Returns empty list if expired or missing."""
    session_file = SESSIONS_DIR / f"{user_key}.json"
    if not session_file.exists():
        return []
    try:
        data = json.loads(session_file.read_text())
        last_active = data.get("last_active", 0)
        if time.time() - last_active > SESSION_TIMEOUT:
            # Session expired — clean up
            session_file.unlink()
            if user_key in _resumption_handles:
                del _resumption_handles[user_key]
            return []
        return data.get("history", [])
    except (json.JSONDecodeError, KeyError):
        return []


def save_session(user_key: str, history: list[dict]):
    """Save session history to JSON file with timestamp."""
    session_file = SESSIONS_DIR / f"{user_key}.json"
    data = {
        "last_active": time.time(),
        "history": history,
    }
    session_file.write_text(json.dumps(data, indent=2))


# ── WebSocket: ESP32 ↔ Gemini Live relay ──────────────────────────────────────

@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket, key: str = ""):
    user = get_user(key, load_users())
    if not user:
        await websocket.close(code=4001)
        return

    await websocket.accept()

    system_prompt = user.get("personality", DEFAULTS["personality"])
    voice_name = user.get("voice", DEFAULTS["voice"]) or "Kore"
    user_key = user["_key"]

    # Register this websocket so /reset can force-close it
    _active_websockets[user_key] = websocket

    # Load conversation history from disk
    history = load_session(user_key)

    # Check for a resumption handle from a previous Gemini session
    print(f"[WS] User {user_key[:8]}... | History turns: {len(history)}", flush=True)

    # Dynamically adjust system prompt based on whether this is a new or continuing game
    if history:
        history_text = "\n".join(
            f"{'Player' if h['role'] == 'user' else 'DM'}: {h['text']}"
            for h in history
        )
        full_prompt = (
            f"{system_prompt}\n\n{DM_CONTINUATION}\n\n"
            f"=== CONVERSATION HISTORY ===\n{history_text}\n=== END HISTORY ==="
        )
    else:
        full_prompt = system_prompt + "\n\n" + DM_NEW_GAME_INTRO

    # Configure Gemini Live session
    try:
        config = types.LiveConnectConfig(
            response_modalities=["AUDIO"],
            system_instruction=types.Content(
                parts=[types.Part(text=full_prompt)]
            ),
            speech_config=types.SpeechConfig(
                voice_config=types.VoiceConfig(
                    prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=voice_name)
                )
            ),
            input_audio_transcription=types.AudioTranscriptionConfig(),
            output_audio_transcription=types.AudioTranscriptionConfig(),
        )
    except Exception as e:
        print(f"[WS] Config error: {e}", flush=True)
        config = types.LiveConnectConfig(
            response_modalities=["AUDIO"],
            system_instruction=types.Content(
                parts=[types.Part(text=full_prompt)]
            ),
            speech_config=types.SpeechConfig(
                voice_config=types.VoiceConfig(
                    prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=voice_name)
                )
            ),
        )

    client = genai.Client()

    try:
        async with client.aio.live.connect(
            model=GEMINI_LIVE_MODEL, config=config
        ) as gemini_session:
            print(f"[WS] Gemini session connected for {user_key[:8]}...", flush=True)

            # Task: forward ESP32 audio → Gemini
            async def esp_to_gemini():
                try:
                    while True:
                        msg = await websocket.receive()
                        if msg["type"] == "websocket.disconnect":
                            break
                        if msg.get("bytes"):
                            await gemini_session.send_realtime_input(
                                audio=types.Blob(
                                    data=msg["bytes"],
                                    mime_type="audio/pcm;rate=16000"
                                )
                            )
                        elif msg.get("text"):
                            data = json.loads(msg["text"])
                            if data.get("type") == "ping":
                                await websocket.send_text(
                                    json.dumps({"type": "pong"})
                                )
                            elif data.get("type") == "end_audio":
                                await gemini_session.send_realtime_input(
                                    audio_stream_end=True
                                )
                except WebSocketDisconnect:
                    pass
                except Exception:
                    pass

            # Task: forward Gemini audio → ESP32
            async def gemini_to_esp():
                # Accumulate transcription fragments into complete turns
                current_user_text = []
                current_ai_text = []

                def flush_user():
                    if current_user_text:
                        combined = " ".join(current_user_text).strip()
                        if combined:
                            history.append({"role": "user", "text": combined})
                        current_user_text.clear()

                def flush_ai():
                    if current_ai_text:
                        combined = " ".join(current_ai_text).strip()
                        if combined:
                            history.append({"role": "assistant", "text": combined})
                        current_ai_text.clear()

                try:
                    async for response in gemini_session.receive():
                        sc = response.server_content
                        if sc is None:
                            continue

                        # Handle interruption
                        if sc.interrupted:
                            flush_ai()
                            save_session(user_key, history)
                            try:
                                await websocket.send_text(
                                    json.dumps({"type": "interrupted"})
                                )
                            except Exception:
                                break
                            continue

                        # Model audio output
                        if sc.model_turn:
                            for part in sc.model_turn.parts:
                                if part.inline_data:
                                    audio = part.inline_data.data
                                    for i in range(0, len(audio), 1024):
                                        chunk = audio[i:i + 1024]
                                        try:
                                            await websocket.send_bytes(chunk)
                                        except Exception:
                                            return
                                        await asyncio.sleep(0.018)

                        # Transcriptions — accumulate fragments
                        if sc.input_transcription:
                            text = sc.input_transcription.text
                            if text and text.strip():
                                # User speaking — flush any pending AI text
                                flush_ai()
                                current_user_text.append(text.strip())
                            try:
                                await websocket.send_text(json.dumps({
                                    "type": "transcript",
                                    "text": text
                                }))
                            except Exception:
                                pass

                        if sc.output_transcription:
                            text = sc.output_transcription.text
                            if text and text.strip():
                                # AI speaking — flush any pending user text
                                flush_user()
                                current_ai_text.append(text.strip())
                            try:
                                await websocket.send_text(json.dumps({
                                    "type": "response",
                                    "text": text
                                }))
                            except Exception:
                                pass

                        # Turn complete — flush and save
                        if hasattr(sc, 'turn_complete') and sc.turn_complete:
                            flush_user()
                            flush_ai()
                            save_session(user_key, history)

                        # Handle go_away — server is about to disconnect
                        if hasattr(response, 'go_away') and response.go_away:
                            flush_user()
                            flush_ai()
                            save_session(user_key, history)
                            try:
                                await websocket.send_text(json.dumps({
                                    "type": "reconnecting",
                                    "message": "Session rotating..."
                                }))
                            except Exception:
                                pass

                except Exception as gemini_err:
                    print(f"[WS] Gemini receive error: {type(gemini_err).__name__}: {gemini_err}", flush=True)
                finally:
                    # Always flush remaining text on disconnect
                    flush_user()
                    flush_ai()
                    # Only save if not reset
                    if user_key not in _reset_flags:
                        save_session(user_key, history)
                    else:
                        _reset_flags.discard(user_key)
                    print(f"[WS] Gemini receive loop ended for {user_key[:8]}... (history: {len(history)} turns)", flush=True)

            # Run both relay tasks concurrently
            esp_task = asyncio.create_task(esp_to_gemini())
            gemini_task = asyncio.create_task(gemini_to_esp())

            done, pending = await asyncio.wait(
                [esp_task, gemini_task],
                return_when=asyncio.FIRST_COMPLETED
            )
            for task in pending:
                task.cancel()

    except WebSocketDisconnect:
        print(f"[WS] Client disconnected for {user_key[:8]}...", flush=True)
    except Exception as e:
        import traceback
        print(f"[WS] Error for {user_key[:8]}...: {e}", flush=True)
        traceback.print_exc()
        try:
            await websocket.send_text(json.dumps({"type": "error", "message": str(e)}))
        except Exception:
            pass
    finally:
        if _active_websockets.get(user_key) is websocket:
            del _active_websockets[user_key]


# ── Static files ──────────────────────────────────────────────────────────────

from starlette.middleware.base import BaseHTTPMiddleware

class NoCacheStaticMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request, call_next):
        response = await call_next(request)
        if request.url.path.endswith(('.js', '.css', '.html')):
            response.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
            response.headers["Pragma"] = "no-cache"
        return response

app.add_middleware(NoCacheStaticMiddleware)

if PUBLIC_DIR.exists():
    app.mount("/", StaticFiles(directory=str(PUBLIC_DIR), html=True), name="static")
else:
    @app.get("/")
    async def no_ui():
        return {"error": f"UI not found at {PUBLIC_DIR}"}


if __name__ == "__main__":
    port = int(os.getenv("PORT", 8765))
    print(f"╔══════════════════════════════════════╗")
    print(f"║   SoloRolls — AI Dungeon Master     ║")
    print(f"║   (Gemini Live API)                  ║")
    print(f"╠══════════════════════════════════════╣")
    print(f"║  Port: {port:<29}║")
    print(f"║  Model: {GEMINI_LIVE_MODEL:<28}║")
    print(f"╚══════════════════════════════════════╝")
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="info")
