const DEFAULTS = {
  name: "User",
  personality:
    "You are a helpful, friendly AI assistant. Keep responses concise and conversational — your answer will be spoken aloud, so avoid markdown, lists, or long explanations.",
  ttsModel: "melotts",
  ttsLang: "en",
  ttsVoice: "luna",
}

// ── Auth ──────────────────────────────────────────────────────────────────────

function isMaster(request, env) {
  return request.headers.get("Authorization") === `Bearer ${env.API_SECRET}`
}

async function getUser(request, env) {
  const header = request.headers.get("Authorization") ?? ""
  const key = header.startsWith("Bearer ") ? header.slice(7) : ""
  if (!key) return null
  if (key === env.API_SECRET) return { ...DEFAULTS, name: "Admin", _isAdmin: true, _key: key }
  const raw = await env.CONFIG.get("user:" + key)
  if (!raw) return null
  return { ...DEFAULTS, ...JSON.parse(raw), _isAdmin: false, _key: key }
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
    lang: config.ttsLang ?? "en",
  })
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

  // Device audio mode → stream audio response
  const audioBuffer = await request.arrayBuffer()
  if (!audioBuffer.byteLength) return new Response("Empty audio", { status: 400 })

  const stt = await env.AI.run("@cf/openai/whisper-large-v3-turbo", {
    audio: [...new Uint8Array(audioBuffer)],
    language: sttLang(user),
  })
  const userText = stt.text?.trim()
  if (!userText) return new Response("Could not transcribe audio", { status: 422 })

  const responseText = await runLLM(userText, user, env)
  if (!responseText) return new Response("LLM returned empty", { status: 500 })

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

// ── UI ────────────────────────────────────────────────────────────────────────

function ui() {
  const AURA_VOICES = [
    "amalthea","andromeda","apollo","arcas","asteria","athena","calliope","clio",
    "delia","electra","ganymede","helios","hera","hermes","io","iris","janus",
    "juno","jupiter","kore","leda","luna","mars","mnemosyne","neptune","orion",
    "orpheus","phoebe","pluto","rhea","saturn","selene","sol","thalia","theia",
    "titan","vesta",
  ]
  const voiceOptions = AURA_VOICES.map((v) => `<option value="${v}">${v}</option>`).join("")

  // Reusable config fields snippet — prefix avoids id collisions in admin edit rows
  const cfgFields = (pfx = "") => `
    <label>Personality</label>
    <textarea id="${pfx}personality" rows="3"></textarea>
    <label>Voice model</label>
    <select id="${pfx}ttsModel" onchange="onModelChange('${pfx}')">
      <option value="melotts">MeloTTS — multi-language</option>
      <option value="aura-2-en">Aura-2 English — streaming</option>
      <option value="aura-2-es">Aura-2 Spanish — streaming</option>
    </select>
    <div id="${pfx}auraOpts" class="hidden">
      <label>Voice</label>
      <select id="${pfx}ttsVoice">${voiceOptions}</select>
    </div>
    <div id="${pfx}meloOpts">
      <label>Language</label>
      <select id="${pfx}ttsLang">
        <option value="en">English</option>
        <option value="es">Spanish</option>
        <option value="fr">French</option>
        <option value="zh">Chinese</option>
        <option value="jp">Japanese</option>
        <option value="ko">Korean</option>
      </select>
    </div>`

  const html = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI-Lite</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{font:15px/1.5 system-ui,sans-serif;background:#0f0f0f;color:#e0e0e0;padding:1rem;max-width:720px;margin:0 auto}
h1{font-size:1.1rem;margin-bottom:1.25rem;color:#fff}
h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;color:#555;margin-bottom:.6rem}
section{background:#1a1a1a;border-radius:8px;padding:1rem;margin-bottom:.75rem}
label{display:block;font-size:.8rem;color:#888;margin-bottom:.25rem;margin-top:.6rem}
label:first-of-type{margin-top:0}
input,textarea,select{width:100%;background:#111;border:1px solid #2a2a2a;border-radius:5px;color:#e0e0e0;padding:.45rem .65rem;font:inherit}
input:focus,textarea:focus,select:focus{outline:none;border-color:#444}
textarea{resize:vertical;min-height:72px}
button{background:#222;border:1px solid #333;border-radius:5px;color:#e0e0e0;padding:.4rem .85rem;font:inherit;cursor:pointer}
button:hover{background:#2a2a2a}
button.primary{background:#1d4ed8;border-color:#2563eb;color:#fff}
button.primary:hover{background:#2563eb}
button.danger{background:#7f1d1d;border-color:#991b1b;color:#fca5a5}
button.danger:hover{background:#991b1b}
button.sm{padding:.25rem .6rem;font-size:.8rem}
.row{display:flex;gap:.5rem;align-items:flex-end;margin-top:.5rem;flex-wrap:wrap}
.row input,.row select{flex:1;min-width:0}
#chat{min-height:80px;max-height:320px;overflow-y:auto;margin-bottom:.65rem;display:flex;flex-direction:column;gap:.4rem}
.msg{padding:.45rem .7rem;border-radius:6px;max-width:88%;font-size:.88rem;line-height:1.4}
.msg.user{background:#1d4ed8;color:#fff;align-self:flex-end}
.msg.ai{background:#252525;align-self:flex-start}
.msg.err{background:#7f1d1d;color:#fca5a5;align-self:flex-start}
.hint{font-size:.75rem;color:#555;margin-top:.35rem}
.flash{font-size:.78rem;color:#4ade80;display:none}
table{width:100%;border-collapse:collapse;font-size:.85rem}
th{text-align:left;color:#555;font-weight:normal;padding:.3rem .5rem;border-bottom:1px solid #222}
td{padding:.35rem .5rem;border-bottom:1px solid #1a1a1a;vertical-align:middle}
.key-val{font-family:monospace;font-size:.78rem;background:#111;padding:.2rem .4rem;border-radius:3px;word-break:break-all}
.edit-row td{padding:.75rem .5rem;background:#111}
.edit-row .row{margin-top:.75rem}
.hidden{display:none}
</style>
</head>
<body>
<h1>AI-Lite</h1>

<section>
  <h2>Auth</h2>
  <label for="apikey">API Key</label>
  <div class="row">
    <input type="password" id="apikey" placeholder="paste your key" autocomplete="off">
    <button onclick="login()">Connect</button>
    <button onclick="logout()" id="logoutBtn" class="hidden">Logout</button>
  </div>
  <div class="hint" id="whoami"></div>
</section>

<section id="chatSection" class="hidden">
  <h2>Chat</h2>
  <div id="chat"></div>
  <label for="msg">Message</label>
  <div class="row">
    <input type="text" id="msg" placeholder="Type something..." onkeydown="if(event.key==='Enter')send()">
    <button class="primary" id="sendBtn" onclick="send()">Send</button>
  </div>
  <div class="hint" id="status"></div>
</section>

<section id="cfgSection" class="hidden">
  <h2>My Config</h2>
  ${cfgFields("")}
  <div class="row" style="margin-top:.75rem">
    <button class="primary" onclick="saveConfig()">Save</button>
    <span class="flash" id="cfgFlash">Saved</span>
  </div>
</section>

<section id="adminSection" class="hidden">
  <h2>Admin — Users</h2>
  <details style="margin-bottom:.75rem">
    <summary style="cursor:pointer;font-size:.85rem;color:#888">Create new user</summary>
    <div style="margin-top:.5rem">
      <label>Name</label>
      <input type="text" id="newName" placeholder="Alice">
      ${cfgFields("new_")}
      <div class="row" style="margin-top:.75rem">
        <button class="primary" onclick="createUser()">Create</button>
      </div>
      <div id="newKeyBox" class="hidden" style="margin-top:.6rem">
        <div style="font-size:.8rem;color:#888;margin-bottom:.3rem">Key (shown once — copy now):</div>
        <div class="key-val" id="newKeyVal"></div>
      </div>
    </div>
  </details>
  <table>
    <thead><tr><th>Name</th><th>Model</th><th>Key</th><th></th></tr></thead>
    <tbody id="userTable"></tbody>
  </table>
</section>

<script>
const VOICES = [${AURA_VOICES.map((v) => `"${v}"`).join(",")}]
const key = () => localStorage.getItem('apikey') || ''
const auth = () => ({'Authorization':'Bearer '+key()})
const jsonAuth = () => ({...auth(),'Content-Type':'application/json'})

function flash(id) {
  const el = document.getElementById(id)
  el.style.display = 'inline'
  setTimeout(() => el.style.display = 'none', 2000)
}

function addMsg(text, role) {
  const el = document.createElement('div')
  el.className = 'msg ' + role
  el.textContent = text
  const chat = document.getElementById('chat')
  chat.appendChild(el)
  chat.scrollTop = chat.scrollHeight
}

function onModelChange(pfx) {
  const m = document.getElementById(pfx+'ttsModel').value
  document.getElementById(pfx+'auraOpts').classList.toggle('hidden', !m.startsWith('aura'))
  document.getElementById(pfx+'meloOpts').classList.toggle('hidden', m.startsWith('aura'))
}

function applyCfg(cfg, pfx='') {
  document.getElementById(pfx+'personality').value = cfg.personality || ''
  document.getElementById(pfx+'ttsModel').value = cfg.ttsModel || 'melotts'
  document.getElementById(pfx+'ttsVoice').value = cfg.ttsVoice || 'luna'
  document.getElementById(pfx+'ttsLang').value = cfg.ttsLang || 'en'
  onModelChange(pfx)
}

function readCfg(pfx='') {
  return {
    personality: document.getElementById(pfx+'personality').value.trim(),
    ttsModel: document.getElementById(pfx+'ttsModel').value,
    ttsVoice: document.getElementById(pfx+'ttsVoice').value,
    ttsLang: document.getElementById(pfx+'ttsLang').value,
  }
}

function logout() {
  localStorage.removeItem('apikey')
  document.getElementById('apikey').value = ''
  document.getElementById('whoami').textContent = ''
  document.getElementById('logoutBtn').classList.add('hidden')
  for (const id of ['chatSection','cfgSection','adminSection'])
    document.getElementById(id).classList.add('hidden')
}

async function login() {
  const k = document.getElementById('apikey').value.trim()
  if (!k) return
  localStorage.setItem('apikey', k)
  await init()
}

async function init() {
  if (!key()) return
  document.getElementById('apikey').value = key()

  const [whoRes, cfgRes] = await Promise.all([
    fetch('/whoami', {headers: auth()}),
    fetch('/config', {headers: auth()}),
  ])

  if (!whoRes.ok) { document.getElementById('whoami').textContent = 'Invalid key'; return }

  const who = await whoRes.json()
  document.getElementById('whoami').textContent = 'Connected as ' + who.name + (who.isAdmin ? ' (admin)' : '')
  document.getElementById('logoutBtn').classList.remove('hidden')
  document.getElementById('chatSection').classList.remove('hidden')
  document.getElementById('cfgSection').classList.toggle('hidden', who.isAdmin)
  document.getElementById('adminSection').classList.toggle('hidden', !who.isAdmin)

  if (!who.isAdmin && cfgRes.ok) applyCfg(await cfgRes.json())
  if (who.isAdmin) loadUsers()
}

async function send() {
  const input = document.getElementById('msg')
  const text = input.value.trim()
  if (!text) return
  input.value = ''
  addMsg(text, 'user')
  document.getElementById('sendBtn').disabled = true
  document.getElementById('status').textContent = 'Thinking...'
  try {
    const res = await fetch('/chat', {method:'POST', headers:jsonAuth(), body:JSON.stringify({text})})
    if (!res.ok) { addMsg('Error '+res.status+': '+await res.text(), 'err'); return }
    const data = await res.json()
    addMsg(data.text, 'ai')
    document.getElementById('status').textContent = 'Playing...'
    const mp3 = Uint8Array.from(atob(data.audio), c => c.charCodeAt(0))
    const audio = new Audio(URL.createObjectURL(new Blob([mp3], {type:'audio/mpeg'})))
    audio.onended = () => document.getElementById('status').textContent = ''
    audio.play()
  } catch(e) {
    addMsg('Error: '+e.message, 'err')
    document.getElementById('status').textContent = ''
  } finally {
    document.getElementById('sendBtn').disabled = false
  }
}

async function saveConfig() {
  const res = await fetch('/config', {method:'POST', headers:jsonAuth(), body:JSON.stringify(readCfg())})
  if (res.ok) flash('cfgFlash')
}

async function loadUsers() {
  const res = await fetch('/admin/keys', {headers: auth()})
  if (!res.ok) return
  const users = await res.json()
  const tbody = document.getElementById('userTable')
  tbody.innerHTML = users.map(u => \`
    <tr id="row-\${u.key}">
      <td>\${u.name}</td>
      <td>\${u.ttsModel||'melotts'}</td>
      <td><span class="key-val">\${u.key}</span></td>
      <td style="white-space:nowrap">
        <button class="sm" onclick="toggleEdit('\${u.key}')">Edit</button>
        <button class="sm danger" onclick="deleteUser('\${u.key}')">Delete</button>
      </td>
    </tr>
    <tr id="edit-\${u.key}" class="edit-row hidden">
      <td colspan="4">
        <div id="ef-\${u.key}">loading...</div>
      </td>
    </tr>
  \`).join('')
}

async function toggleEdit(k) {
  const row = document.getElementById('edit-'+k)
  if (!row.classList.contains('hidden')) { row.classList.add('hidden'); return }
  row.classList.remove('hidden')
  const div = document.getElementById('ef-'+k)
  if (div.dataset.loaded) return
  div.dataset.loaded = '1'

  const res = await fetch('/admin/keys/'+k, {headers: auth()})
  const cfg = await res.json()
  const pfx = 'u'+k+'_'

  div.innerHTML = \`
    <label>Name</label>
    <input id="\${pfx}name" value="\${cfg.name||''}">
    <label>Personality</label>
    <textarea id="\${pfx}personality" rows="3"></textarea>
    <label>Voice model</label>
    <select id="\${pfx}ttsModel" onchange="onModelChange('\${pfx}')">
      <option value="melotts">MeloTTS</option>
      <option value="aura-2-en">Aura-2 English</option>
      <option value="aura-2-es">Aura-2 Spanish</option>
    </select>
    <div id="\${pfx}auraOpts" class="hidden">
      <label>Voice</label>
      <select id="\${pfx}ttsVoice">\${VOICES.map(v=>\`<option>\${v}</option>\`).join('')}</select>
    </div>
    <div id="\${pfx}meloOpts">
      <label>Language</label>
      <select id="\${pfx}ttsLang">
        <option value="en">English</option><option value="es">Spanish</option>
        <option value="fr">French</option><option value="zh">Chinese</option>
        <option value="jp">Japanese</option><option value="ko">Korean</option>
      </select>
    </div>
    <div class="row">
      <button class="primary sm" onclick="saveUser('\${k}','\${pfx}')">Save</button>
      <span class="flash" id="\${pfx}flash">Saved</span>
    </div>
  \`
  applyCfg(cfg, pfx)
  document.getElementById(pfx+'name').value = cfg.name || ''
}

async function saveUser(k, pfx) {
  const body = {
    name: document.getElementById(pfx+'name').value.trim(),
    ...readCfg(pfx),
  }
  const res = await fetch('/admin/keys/'+k, {method:'PUT', headers:jsonAuth(), body:JSON.stringify(body)})
  if (res.ok) { flash(pfx+'flash'); loadUsers() }
}

async function createUser() {
  const name = document.getElementById('newName').value.trim()
  if (!name) return
  const body = { name, ...readCfg('new_') }
  const res = await fetch('/admin/keys', {method:'POST', headers:jsonAuth(), body:JSON.stringify(body)})
  if (!res.ok) return
  const data = await res.json()
  document.getElementById('newKeyVal').textContent = data.key
  document.getElementById('newKeyBox').classList.remove('hidden')
  document.getElementById('newName').value = ''
  loadUsers()
}

async function deleteUser(k) {
  if (!confirm('Delete user '+k.slice(0,8)+'...?')) return
  await fetch('/admin/keys/'+k, {method:'DELETE', headers:auth()})
  loadUsers()
}

init()
</script>
</body>
</html>`

  return new Response(html, { headers: { "Content-Type": "text/html;charset=utf-8" } })
}

// ── Router ────────────────────────────────────────────────────────────────────

export default {
  async fetch(request, env) {
    const url = new URL(request.url)
    const path = url.pathname

    if (request.method === "GET" && path === "/") return ui()

    if (path.startsWith("/admin/")) return handleAdmin(request, env)

    const user = await getUser(request, env)
    if (!user) return new Response("Unauthorized", { status: 401 })

    if (path === "/whoami" && request.method === "GET") {
      return Response.json({ name: user.name, isAdmin: user._isAdmin })
    }

    if (path === "/config") return handleConfig(request, user, env)

    if (path === "/chat" && request.method === "POST") return handleChat(request, user, env)

    return new Response("Not Found", { status: 404 })
  },
}
