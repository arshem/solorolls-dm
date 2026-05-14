#!/usr/bin/env python3
"""
Local AI voice assistant server.
Drop-in replacement for the Cloudflare Worker — same API, same web UI,
local HuggingFace models instead of CF AI, users.json instead of KV.

Usage:
    pip install -r requirements.txt
    API_SECRET=<your-admin-key> python server.py

The API_SECRET env var must match the "admin" value in users.json.
"""

import asyncio
import base64
import json
import os
import secrets
import threading
from pathlib import Path

import uvicorn
from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles

import models

USERS_FILE = Path(__file__).parent / "users.json"
PUBLIC_DIR = (Path(__file__).parent.parent / "worker" / "public").resolve()

DEFAULTS = {
    "name": "User",
    "personality": (
        "You are a helpful, friendly AI assistant. Keep responses concise and "
        "conversational — your answer will be spoken aloud, so avoid markdown, "
        "lists, or long explanations."
    ),
    "ttsModel": "en",
    "ttsVoice": "af_luna",
}

ALLOWED_FIELDS = {"name", "personality", "ttsModel", "ttsLang", "ttsVoice"}

_users_lock = threading.Lock()


# ── Users store ───────────────────────────────────────────────────────────────

def load_users() -> dict:
    with _users_lock:
        if USERS_FILE.exists():
            return json.loads(USERS_FILE.read_text())
        return {"admin": os.getenv("API_SECRET", secrets.token_hex(32))}


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

app = FastAPI()


# ── whoami ────────────────────────────────────────────────────────────────────

@app.get("/whoami")
async def whoami(request: Request):
    user = require_user(request)
    return {"name": user["name"], "isAdmin": user["_isAdmin"]}


# ── config ────────────────────────────────────────────────────────────────────

@app.get("/config")
async def get_config(request: Request):
    user = require_user(request)
    return {k: v for k, v in user.items() if not k.startswith("_")}


@app.post("/config")
async def post_config(request: Request):
    user = require_user(request)
    if user["_isAdmin"]:
        raise HTTPException(400, "Admin has no personal config — create a user account")
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


@app.get("/admin/keys/{key}")
async def admin_get(key: str, request: Request):
    require_admin(request)
    users = load_users()
    cfg = users.get(key)
    if cfg is None:
        raise HTTPException(404, "Not Found")
    return {"key": key, **{**DEFAULTS, **cfg}}


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


@app.put("/admin/keys/{key}")
async def admin_update(key: str, request: Request):
    require_admin(request)
    users = load_users()
    if key not in users or key == "admin":
        raise HTTPException(404, "Not Found")
    body = await request.json()
    updated = {**DEFAULTS, **users[key]}
    for f in ALLOWED_FIELDS:
        if f in body:
            updated[f] = body[f]
    users[key] = updated
    save_users(users)
    return Response("OK")


@app.delete("/admin/keys/{key}")
async def admin_delete(key: str, request: Request):
    require_admin(request)
    users = load_users()
    if key not in users or key == "admin":
        raise HTTPException(404, "Not Found")
    del users[key]
    save_users(users)
    return Response("OK")


# ── chat ──────────────────────────────────────────────────────────────────────

@app.post("/chat")
async def chat(request: Request):
    user = require_user(request)
    content_type = request.headers.get("content-type", "")
    accept = request.headers.get("accept", "")
    system = user.get("personality", DEFAULTS["personality"])
    voice = models.resolve_voice(user)

    if "application/json" in content_type:
        body = await request.json()
        text = (body.get("text") or "").strip()
        if not text:
            raise HTTPException(400, "Missing text")
        response_text = await asyncio.to_thread(models.respond, text, system)
        mp3 = await asyncio.to_thread(models.speak, response_text, voice)
        return {"text": response_text, "audio": base64.b64encode(mp3).decode()}

    wav = await request.body()
    if not wav:
        raise HTTPException(400, "Empty audio")

    lang = models.stt_lang(user)
    transcript = await asyncio.to_thread(models.transcribe, wav, lang)
    if not transcript:
        raise HTTPException(422, "Could not transcribe audio")

    response_text = await asyncio.to_thread(models.respond, transcript, system)
    mp3 = await asyncio.to_thread(models.speak, response_text, voice)

    if "application/json" in accept:
        return {"input": transcript, "text": response_text, "audio": base64.b64encode(mp3).decode()}

    return Response(content=mp3, media_type="audio/mpeg")


# ── WebSocket ─────────────────────────────────────────────────────────────────

@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket, key: str = ""):
    user = get_user(key, load_users())
    if not user:
        await websocket.close(code=4001)
        return

    await websocket.accept()
    audio_chunks: list[bytes] = []
    system = user.get("personality", DEFAULTS["personality"])
    voice = models.resolve_voice(user)

    try:
        while True:
            msg = await websocket.receive()

            if msg["type"] == "websocket.disconnect":
                break

            if msg.get("bytes"):
                audio_chunks.append(msg["bytes"])
                continue

            if not msg.get("text"):
                continue

            try:
                data = json.loads(msg["text"])
            except json.JSONDecodeError:
                continue

            if data.get("type") != "done" or not audio_chunks:
                continue

            wav = b"".join(audio_chunks)
            audio_chunks.clear()

            try:
                lang = models.stt_lang(user)
                transcript = await asyncio.to_thread(models.transcribe, wav, lang)
                if not transcript:
                    await websocket.send_text(json.dumps({"type": "error", "message": "Could not transcribe audio"}))
                    continue
                await websocket.send_text(json.dumps({"type": "transcript", "text": transcript}))

                response_text = await asyncio.to_thread(models.respond, transcript, system)
                if not response_text:
                    await websocket.send_text(json.dumps({"type": "error", "message": "LLM returned empty"}))
                    continue
                await websocket.send_text(json.dumps({"type": "response", "text": response_text}))

                mp3 = await asyncio.to_thread(models.speak, response_text, voice)
                try:
                    await websocket.send_text(json.dumps({"type": "audio_start"}))
                    for i in range(0, len(mp3), 4096):
                        await websocket.send_bytes(mp3[i:i + 4096])
                    await websocket.send_text(json.dumps({"type": "audio_end"}))
                except Exception:
                    break

            except Exception as e:
                try:
                    await websocket.send_text(json.dumps({"type": "error", "message": str(e)}))
                except Exception:
                    break

    except WebSocketDisconnect:
        pass


# ── Static files (must come last — catches everything else) ───────────────────

if PUBLIC_DIR.exists():
    app.mount("/", StaticFiles(directory=str(PUBLIC_DIR), html=True), name="static")
else:
    @app.get("/")
    async def no_ui():
        return {"error": f"UI not found at {PUBLIC_DIR}"}


if __name__ == "__main__":
    port = int(os.getenv("PORT", 8787))
    print(f"Serving UI from {PUBLIC_DIR}")
    print(f"Users file: {USERS_FILE}")
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="info")
