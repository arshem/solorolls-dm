const DEFAULTS = {
  name: "User",
  personality:
    "You are a helpful, friendly AI assistant. Keep responses concise and conversational — your answer will be spoken aloud, so avoid markdown, lists, or long explanations.",
  ttsModel: "aura-2-en",
  ttsVoice: "luna",
}

// ── Auth ──────────────────────────────────────────────────────────────────────

function isMaster(request, env) {
  return request.headers.get("Authorization") === `Bearer ${env.API_SECRET}`
}

async function getUserByKey(key, env) {
  if (!key) return null
  if (key === env.API_SECRET) return { ...DEFAULTS, name: "Admin", _isAdmin: true, _key: key }
  const raw = await env.CONFIG.get("user:" + key)
  if (!raw) return null
  return { ...DEFAULTS, ...JSON.parse(raw), _isAdmin: false, _key: key }
}

async function getUser(request, env) {
  const header = request.headers.get("Authorization") ?? ""
  const key = header.startsWith("Bearer ") ? header.slice(7) : ""
  return getUserByKey(key, env)
}

// ── Helpers ───────────────────────────────────────────────────────────────────

function fromBase64(b64) {
  return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))
}

function toBase64(bytes) {
  let binary = ""
  for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i])
  return btoa(binary)
}

async function streamToBytes(stream) {
  const chunks = []
  let total = 0
  const reader = stream.getReader()
  while (true) {
    const { done, value } = await reader.read()
    if (done) break
    chunks.push(value)
    total += value.length
  }
  const out = new Uint8Array(total)
  let offset = 0
  for (const chunk of chunks) { out.set(chunk, offset); offset += chunk.length }
  return out
}

function generateKey() {
  return Array.from(crypto.getRandomValues(new Uint8Array(24)))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("")
}

// ── TTS ───────────────────────────────────────────────────────────────────────

const AURA_MODELS = {
  "aura-2-en": "@cf/deepgram/aura-2-en",
  "aura-2-es": "@cf/deepgram/aura-2-es",
}

// Returns { stream } for streaming response, { bytes } for buffered.
// wantStream=true only for device path.
async function runTTS(text, config, env, wantStream = false) {
  const model = config.ttsModel ?? "melotts"
  const cfModel = AURA_MODELS[model]

  if (cfModel) {
    const stream = await env.AI.run(cfModel, {
      text,
      speaker: config.ttsVoice ?? "luna",
      encoding: "mp3",
    })
    if (wantStream) return { stream }
    return { bytes: await streamToBytes(stream) }
  }

  // melotts — returns base64, no native streaming
  const result = await env.AI.run("@cf/myshell-ai/melotts", {
    prompt: text,
    lang: (config.ttsLang ?? "en").toUpperCase(),
  })
  if (!result?.audio) throw new Error(`MeloTTS returned no audio (lang=${config.ttsLang})`)
  const bytes = fromBase64(result.audio)
  if (wantStream) {
    const stream = new ReadableStream({
      start(c) { c.enqueue(bytes); c.close() },
    })
    return { stream }
  }
  return { bytes }
}

// ── LLM ───────────────────────────────────────────────────────────────────────

async function runLLM(userText, config, env) {
  const result = await env.AI.run("@cf/meta/llama-3.1-8b-instruct", {
    messages: [
      { role: "system", content: config.personality ?? DEFAULTS.personality },
      { role: "user", content: userText },
    ],
    max_tokens: 256,
  })
  return result.response?.trim() ?? ""
}

// STT language: derived from ttsModel so STT matches TTS language
function sttLang(config) {
  if (config.ttsModel === "aura-2-es") return "es"
  return config.ttsLang ?? "en"
}

// ── Chat ──────────────────────────────────────────────────────────────────────

async function handleChat(request, user, env) {
  const contentType = request.headers.get("Content-Type") ?? ""

  if (contentType.includes("application/json")) {
    // UI text mode → return JSON {text, audio}
    let body
    try { body = await request.json() } catch { return new Response("Invalid JSON", { status: 400 }) }
    const userText = body.text?.trim()
    if (!userText) return new Response("Missing text", { status: 400 })

    const responseText = await runLLM(userText, user, env)
    if (!responseText) return new Response("LLM returned empty", { status: 500 })

    const { bytes } = await runTTS(responseText, user, env, false)
    return Response.json({ text: responseText, audio: toBase64(bytes) })
  }

  // Device/browser audio mode — binary body
  const audioBuffer = await request.arrayBuffer()
  if (!audioBuffer.byteLength) return new Response("Empty audio", { status: 400 })

  const stt = await env.AI.run("@cf/openai/whisper-large-v3-turbo", {
    audio: toBase64(new Uint8Array(audioBuffer)),
    language: sttLang(user),
  })
  const userText = stt.text?.trim()
  if (!userText) return new Response("Could not transcribe audio", { status: 422 })

  const responseText = await runLLM(userText, user, env)
  if (!responseText) return new Response("LLM returned empty", { status: 500 })

  // Browser requests JSON (includes transcription); device gets raw stream
  const wantJson = (request.headers.get("Accept") ?? "").includes("application/json")
  if (wantJson) {
    const { bytes } = await runTTS(responseText, user, env, false)
    return Response.json({ input: userText, text: responseText, audio: toBase64(bytes) })
  }

  const { stream } = await runTTS(responseText, user, env, true)
  return new Response(stream, { headers: { "Content-Type": "audio/mpeg" } })
}

// ── Admin ─────────────────────────────────────────────────────────────────────

async function handleAdmin(request, env) {
  if (!isMaster(request, env)) return new Response("Forbidden", { status: 403 })

  const parts = new URL(request.url).pathname.split("/").filter(Boolean)
  // parts: ["admin", "keys"] or ["admin", "keys", ":key"]

  if (parts[1] !== "keys") return new Response("Not Found", { status: 404 })

  // GET /admin/keys — list all users
  if (request.method === "GET" && !parts[2]) {
    const list = await env.CONFIG.list({ prefix: "user:" })
    const users = await Promise.all(
      list.keys.map(async ({ name: kvKey }) => {
        const key = kvKey.slice(5)
        const raw = await env.CONFIG.get(kvKey)
        const cfg = raw ? JSON.parse(raw) : {}
        return { key, ...{ ...DEFAULTS, ...cfg } }
      })
    )
    return Response.json(users)
  }

  // GET /admin/keys/:key — full config for one user
  if (request.method === "GET" && parts[2]) {
    const raw = await env.CONFIG.get("user:" + parts[2])
    if (!raw) return new Response("Not Found", { status: 404 })
    return Response.json({ key: parts[2], ...DEFAULTS, ...JSON.parse(raw) })
  }

  // POST /admin/keys — create user
  if (request.method === "POST" && !parts[2]) {
    let body = {}
    try { body = await request.json() } catch {}
    const key = generateKey()
    const config = { ...DEFAULTS, ...body }
    delete config._isAdmin; delete config._key
    await env.CONFIG.put("user:" + key, JSON.stringify(config))
    return Response.json({ key, ...config })
  }

  // PUT /admin/keys/:key — update user config
  if (request.method === "PUT" && parts[2]) {
    const raw = await env.CONFIG.get("user:" + parts[2])
    if (!raw) return new Response("Not Found", { status: 404 })
    let body
    try { body = await request.json() } catch { return new Response("Invalid JSON", { status: 400 }) }
    const allowed = ["name", "personality", "ttsModel", "ttsLang", "ttsVoice"]
    const updated = { ...DEFAULTS, ...JSON.parse(raw) }
    for (const k of allowed) if (body[k] !== undefined) updated[k] = body[k]
    await env.CONFIG.put("user:" + parts[2], JSON.stringify(updated))
    return new Response("OK")
  }

  // DELETE /admin/keys/:key — delete user
  if (request.method === "DELETE" && parts[2]) {
    await env.CONFIG.delete("user:" + parts[2])
    return new Response("OK")
  }

  return new Response("Not Found", { status: 404 })
}

// ── Config ────────────────────────────────────────────────────────────────────

async function handleConfig(request, user, env) {
  const { _isAdmin, _key, ...cfg } = user

  if (request.method === "GET") return Response.json(cfg)

  if (request.method === "POST") {
    if (_isAdmin) return new Response("Admin has no personal config — create a user account", { status: 400 })
    let body
    try { body = await request.json() } catch { return new Response("Invalid JSON", { status: 400 }) }
    const allowed = ["personality", "ttsModel", "ttsLang", "ttsVoice", "name"]
    const updated = { ...cfg }
    for (const k of allowed) if (body[k] !== undefined) updated[k] = body[k]
    await env.CONFIG.put("user:" + _key, JSON.stringify(updated))
    return new Response("OK")
  }

  return new Response("Method Not Allowed", { status: 405 })
}

// ── WebSocket ─────────────────────────────────────────────────────────────────

async function handleWebSocket(request, env) {
  if (request.headers.get("Upgrade") !== "websocket") {
    return new Response("Expected WebSocket upgrade", { status: 426 })
  }

  const key = new URL(request.url).searchParams.get("key") || ""
  const user = await getUserByKey(key, env)
  if (!user) return new Response("Unauthorized", { status: 401 })

  const [client, server] = Object.values(new WebSocketPair())
  server.accept()

  const audioChunks = []
  let processing = false
  let closed = false

  server.addEventListener("close", () => { closed = true })
  server.addEventListener("error", () => { closed = true })

  server.addEventListener("message", async (event) => {
    if (closed) return

    if (event.data instanceof ArrayBuffer) {
      if (!processing) audioChunks.push(new Uint8Array(event.data))
      return
    }

    let msg
    try { msg = JSON.parse(event.data) } catch { return }
    if (msg.type !== "done" || processing || audioChunks.length === 0) return

    processing = true
    try {
      const totalLen = audioChunks.reduce((s, c) => s + c.length, 0)
      const audio = new Uint8Array(totalLen)
      let offset = 0
      for (const chunk of audioChunks) { audio.set(chunk, offset); offset += chunk.length }
      audioChunks.length = 0

      const stt = await env.AI.run("@cf/openai/whisper-large-v3-turbo", {
        audio: toBase64(audio),
        language: sttLang(user),
      })
      const userText = stt.text?.trim()
      if (!userText) {
        if (!closed) server.send(JSON.stringify({ type: "error", message: "Could not transcribe audio" }))
        return
      }
      if (!closed) server.send(JSON.stringify({ type: "transcript", text: userText }))

      const responseText = await runLLM(userText, user, env)
      if (!responseText) {
        if (!closed) server.send(JSON.stringify({ type: "error", message: "LLM returned empty" }))
        return
      }
      if (!closed) server.send(JSON.stringify({ type: "response", text: responseText }))

      const { stream } = await runTTS(responseText, user, env, true)
      if (!closed) server.send(JSON.stringify({ type: "audio_start" }))
      const reader = stream.getReader()
      while (!closed) {
        const { done, value } = await reader.read()
        if (done) break
        server.send(value)
      }
      if (!closed) server.send(JSON.stringify({ type: "audio_end" }))
    } catch (e) {
      if (!closed) server.send(JSON.stringify({ type: "error", message: e.message }))
    } finally {
      processing = false
    }
  })

  return new Response(null, { status: 101, webSocket: client })
}

// ── Router ────────────────────────────────────────────────────────────────────

async function handleApi(request, env, ctx) {
  const url = new URL(request.url)
  const path = url.pathname

  if (path === "/ws") return handleWebSocket(request, env)

  if (path === "/firmware.bin") {
    const LATEST = "https://github.com/konsumer/ailite-cf/releases/latest/download/firmware.factory.bin"
    // First redirect gives us the versioned URL (e.g. .../download/v1.2.3/firmware.factory.bin)
    const redir = await fetch(LATEST, { redirect: "manual" })
    const location = redir.headers.get("Location") ?? LATEST
    const versionMatch = location.match(/\/download\/(v[^/]+)\//)
    const version = versionMatch ? versionMatch[1] : "unknown"
    const headers = {
      "Content-Type": "application/octet-stream",
      "Content-Disposition": `attachment; filename="firmware-${version}.factory.bin"`,
      "X-Firmware-Version": version,
      "Cache-Control": "public, max-age=3600",
      "Access-Control-Allow-Origin": "*"
    }
    if (request.method === "HEAD") return new Response(null, { headers })
    const bin = await fetch(location)
    if (!bin.ok) return new Response("Firmware download failed", { status: 502 })
    return new Response(bin.body, { headers })
  }

  const user = await getUser(request, env)
  if (!user) return new Response("Unauthorized", { status: 401 })

  if (path.startsWith("/admin/")) return handleAdmin(request, env)

  if (path === "/whoami" && request.method === "GET") {
    return Response.json({ name: user.name, isAdmin: user._isAdmin })
  }

  if (path === "/config") return handleConfig(request, user, env)

  if (path === "/chat" && request.method === "POST") return handleChat(request, user, env)

  return new Response("Not Found", { status: 404 })
}

const API_PATHS = ["/admin/", "/whoami", "/config", "/chat", "/ws", "/firmware.bin"]

export default {
  async fetch(request, env, ctx) {
    const path = new URL(request.url).pathname
    if (API_PATHS.some((p) => path === p || path.startsWith(p))) {
      return handleApi(request, env, ctx)
    }
    const assetResponse = await env.ASSETS.fetch(request)
    if (assetResponse.status === 404) {
      return handleApi(request, env, ctx)
    }
    return assetResponse
  },
}
