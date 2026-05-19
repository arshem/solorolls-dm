const key = () => localStorage.getItem('apikey') || ''
const auth = () => ({ Authorization: 'Bearer ' + key() })
const jsonAuth = () => ({ ...auth(), 'Content-Type': 'application/json' })

let ws = null
let audioCtx = null
let nextPlayTime = 0
const SAMPLE_RATE = 24000 // Gemini outputs 24kHz PCM

// Accumulate transcription fragments into complete messages
let currentUserMsg = null
let currentAiMsg = null

// ── Wake Lock & Keep-Alive ────────────────────────────────────────────────────

let wakeLock = null

async function requestWakeLock() {
  if (!('wakeLock' in navigator)) return
  try {
    wakeLock = await navigator.wakeLock.request('screen')
    wakeLock.addEventListener('release', () => { wakeLock = null })
  } catch (e) {
    // Wake lock request failed (e.g. low battery, tab not visible)
  }
}

function releaseWakeLock() {
  if (wakeLock) {
    wakeLock.release()
    wakeLock = null
  }
}

// Re-acquire wake lock when page becomes visible again (browser releases it on hide)
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && micStream) {
    requestWakeLock()
  }
})

// Silent audio oscillator to keep the audio session alive on mobile
let keepAliveOsc = null
let keepAliveGain = null

function startAudioKeepAlive() {
  ensureAudioCtx()
  if (keepAliveOsc) return
  keepAliveOsc = audioCtx.createOscillator()
  keepAliveGain = audioCtx.createGain()
  keepAliveOsc.frequency.value = 1 // inaudible 1Hz
  keepAliveGain.gain.value = 0.001 // essentially silent
  keepAliveOsc.connect(keepAliveGain)
  keepAliveGain.connect(audioCtx.destination)
  keepAliveOsc.start()
}

function stopAudioKeepAlive() {
  if (keepAliveOsc) {
    keepAliveOsc.stop()
    keepAliveOsc.disconnect()
    keepAliveOsc = null
  }
  if (keepAliveGain) {
    keepAliveGain.disconnect()
    keepAliveGain = null
  }
}

function flash(id) {
  const el = document.getElementById(id)
  el.style.display = 'inline'
  setTimeout(() => (el.style.display = 'none'), 2000)
}

function addMsg(text, role) {
  const el = document.createElement('div')
  el.className = 'msg ' + role
  el.textContent = text
  const chat = document.getElementById('chat')
  chat.appendChild(el)
  chat.scrollTop = chat.scrollHeight
  return el
}

function appendToMsg(text, role) {
  if (role === 'user') {
    if (!currentUserMsg || !currentUserMsg.isConnected) {
      currentUserMsg = addMsg(text, role)
    } else {
      currentUserMsg.textContent += text
    }
    document.getElementById('chat').scrollTop = document.getElementById('chat').scrollHeight
    return currentUserMsg
  } else {
    if (!currentAiMsg || !currentAiMsg.isConnected) {
      currentAiMsg = addMsg(text, role)
    } else {
      currentAiMsg.textContent += text
    }
    document.getElementById('chat').scrollTop = document.getElementById('chat').scrollHeight
    return currentAiMsg
  }
}

function finalizeMessages() {
  currentUserMsg = null
  currentAiMsg = null
}

// ── UI Navigation ─────────────────────────────────────────────────────────────

function showScreen(id) {
  document.querySelectorAll('.screen').forEach(s => s.classList.add('hidden'))
  document.getElementById(id).classList.remove('hidden')
}

function openSettings() {
  document.getElementById('settingsModal').classList.remove('hidden')
  loadConfig()
}

function closeSettings() {
  document.getElementById('settingsModal').classList.add('hidden')
}

// ── Auth ──────────────────────────────────────────────────────────────────────

function logout() {
  localStorage.removeItem('apikey')
  document.getElementById('apikey').value = ''
  document.getElementById('whoami').textContent = ''
  disconnectWS()
  showScreen('loginScreen')
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

  const whoRes = await fetch('/whoami', { headers: auth() })

  if (!whoRes.ok) {
    document.getElementById('whoami').textContent = 'Invalid key'
    return
  }

  const who = await whoRes.json()
  showScreen('chatScreen')

  // Show admin section in settings if admin
  document.getElementById('adminSettings').classList.toggle('hidden', !who.isAdmin)
  if (who.isAdmin) loadUsers()

  // Load existing conversation history into chat
  await loadHistory()

  connectWS()
}

// ── WebSocket connection ──────────────────────────────────────────────────────

let reconnectTimer = null
let heartbeatTimer = null
let reconnectAttempts = 0
const MAX_RECONNECT_DELAY = 30000

function connectWS() {
  disconnectWS()

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  const url = `${proto}//${location.host}/ws?key=${encodeURIComponent(key())}`

  ws = new WebSocket(url)
  ws.binaryType = 'arraybuffer'

  ws.onopen = () => {
    reconnectAttempts = 0
    document.getElementById('status').textContent = 'Connected — tap mic to talk'
    document.getElementById('recBtn').disabled = false
    startHeartbeat()
  }

  ws.onmessage = (event) => {
    if (event.data instanceof ArrayBuffer) {
      playPCM(event.data)
    } else {
      try {
        const msg = JSON.parse(event.data)
        if (msg.type === 'pong') {
          // heartbeat response
        } else if (msg.type === 'transcript') {
          currentAiMsg = null
          appendToMsg(msg.text, 'user')
        } else if (msg.type === 'response') {
          currentUserMsg = null
          appendToMsg(msg.text, 'ai')
        } else if (msg.type === 'interrupted') {
          finalizeMessages()
          stopPlayback()
          document.getElementById('status').textContent = micStream
            ? 'Listening…'
            : 'Connected — tap mic to talk'
        } else if (msg.type === 'error') {
          finalizeMessages()
          addMsg('Error: ' + msg.message, 'err')
        }
      } catch (e) {
        // ignore parse errors
      }
    }
  }

  ws.onclose = (event) => {
    stopHeartbeat()
    ws = null
    document.getElementById('recBtn').disabled = true

    if (event.code === 4001) {
      document.getElementById('whoami').textContent = 'Invalid key'
      showScreen('loginScreen')
      return
    }

    if (event.code === 4002) {
      document.getElementById('status').textContent = 'Starting new game...'
      return
    }

    // Silently reconnect — only show disconnect status if retries pile up
    scheduleReconnect()
  }

  ws.onerror = () => {
    document.getElementById('status').textContent = 'Connection error'
  }
}

function scheduleReconnect() {
  if (reconnectTimer) return
  const delay = Math.min(1000 * Math.pow(2, reconnectAttempts), MAX_RECONNECT_DELAY)
  reconnectAttempts++
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null
    if (!ws && key()) {
      // Only show disconnect status after multiple failed attempts
      if (reconnectAttempts > 2) {
        document.getElementById('status').textContent = `Reconnecting (attempt ${reconnectAttempts})...`
      }
      connectWS()
    }
  }, delay)
}

function startHeartbeat() {
  stopHeartbeat()
  heartbeatTimer = setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'ping' }))
    }
  }, 25000)
}

function stopHeartbeat() {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer)
    heartbeatTimer = null
  }
}

function disconnectWS() {
  stopHeartbeat()
  if (reconnectTimer) {
    clearTimeout(reconnectTimer)
    reconnectTimer = null
  }
  reconnectAttempts = 0
  if (ws) {
    ws.close()
    ws = null
  }
}

// ── Audio playback (24kHz PCM) ────────────────────────────────────────────────

function ensureAudioCtx() {
  if (!audioCtx) {
    audioCtx = new AudioContext({ sampleRate: SAMPLE_RATE })
  }
  if (audioCtx.state === 'suspended') {
    audioCtx.resume()
  }
}

function playPCM(arrayBuffer) {
  ensureAudioCtx()

  const int16 = new Int16Array(arrayBuffer)
  const float32 = new Float32Array(int16.length)
  for (let i = 0; i < int16.length; i++) {
    float32[i] = int16[i] / 32768
  }

  const buffer = audioCtx.createBuffer(1, float32.length, SAMPLE_RATE)
  buffer.getChannelData(0).set(float32)

  const source = audioCtx.createBufferSource()
  source.buffer = buffer
  source.connect(audioCtx.destination)

  const now = audioCtx.currentTime
  if (nextPlayTime < now) {
    nextPlayTime = now
  }
  source.start(nextPlayTime)
  nextPlayTime += buffer.duration
}

function stopPlayback() {
  // Reset scheduling time so next audio plays immediately
  // Don't close audioCtx — closing it requires a user gesture to recreate
  nextPlayTime = 0
}

// ── Microphone streaming ──────────────────────────────────────────────────────

let micStream = null
let micProcessor = null
let micSource = null
let micCtx = null

async function toggleRec() {
  if (micStream) {
    stopMic()
    return
  }
  await startMic()
}

async function startMic() {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    document.getElementById('status').textContent = 'Not connected'
    return
  }

  try {
    micStream = await navigator.mediaDevices.getUserMedia({ audio: { sampleRate: 16000, channelCount: 1 } })
  } catch (e) {
    document.getElementById('status').textContent = 'Mic access denied'
    return
  }

  // Keep screen awake and audio session alive while mic is active
  requestWakeLock()
  startAudioKeepAlive()

  ensureAudioCtx()

  micCtx = new AudioContext({ sampleRate: 16000 })
  micSource = micCtx.createMediaStreamSource(micStream)

  micProcessor = micCtx.createScriptProcessor(4096, 1, 1)
  micProcessor.onaudioprocess = (e) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    const float32 = e.inputBuffer.getChannelData(0)
    const int16 = new Int16Array(float32.length)
    for (let i = 0; i < float32.length; i++) {
      const s = Math.max(-1, Math.min(1, float32[i]))
      int16[i] = s < 0 ? s * 0x8000 : s * 0x7FFF
    }
    ws.send(int16.buffer)
  }

  micSource.connect(micProcessor)
  micProcessor.connect(micCtx.destination)

  document.getElementById('recBtn').classList.add('recording')
  document.getElementById('status').textContent = 'Listening…'
}

function stopMic() {
  if (micProcessor) {
    micProcessor.disconnect()
    micProcessor = null
  }
  if (micSource) {
    micSource.disconnect()
    micSource = null
  }
  if (micCtx) {
    micCtx.close()
    micCtx = null
  }
  if (micStream) {
    micStream.getTracks().forEach((t) => t.stop())
    micStream = null
  }

  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type: 'end_audio' }))
  }

  // Release screen wake lock and stop audio keepalive
  releaseWakeLock()
  stopAudioKeepAlive()

  document.getElementById('recBtn').classList.remove('recording')
  document.getElementById('status').textContent = 'Connected — tap mic to talk'
}

// ── Game actions ──────────────────────────────────────────────────────────────

async function loadHistory() {
  try {
    const res = await fetch('/history', { headers: auth() })
    if (!res.ok) return
    const data = await res.json()
    if (!data.history || data.history.length === 0) return

    const chat = document.getElementById('chat')
    chat.innerHTML = ''
    for (const turn of data.history) {
      const role = turn.role === 'user' ? 'user' : 'ai'
      addMsg(turn.text, role)
    }
  } catch (e) {
    // silently fail — chat just starts empty
  }
}

async function newGame() {
  if (!confirm('Start a new game? This will erase all conversation history.')) return
  await fetch('/reset', { method: 'POST', headers: auth() })
  document.getElementById('chat').innerHTML = ''
  await new Promise(r => setTimeout(r, 500))
  disconnectWS()
  connectWS()
}

async function downloadGame() {
  const res = await fetch('/history', { headers: auth() })
  if (!res.ok) { alert('No game history found'); return }
  const data = await res.json()
  if (!data.history || data.history.length === 0) { alert('No game history found'); return }

  let text = '=== SoloRolls DM — Game Transcript ===\n\n'
  for (const turn of data.history) {
    const label = turn.role === 'user' ? 'Player' : 'DM'
    text += `${label}: ${turn.text}\n\n`
  }

  const blob = new Blob([text], { type: 'text/plain' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = 'solorolls-game.txt'
  a.click()
  URL.revokeObjectURL(url)
}

// ── Config ────────────────────────────────────────────────────────────────────

async function saveConfig() {
  const body = {
    name: document.getElementById('name').value.trim(),
    personality: document.getElementById('personality').value.trim(),
    voice: document.getElementById('voice').value.trim()
  }
  const res = await fetch('/config', { method: 'POST', headers: jsonAuth(), body: JSON.stringify(body) })
  if (res.ok) flash('cfgFlash')
}

async function loadConfig() {
  const res = await fetch('/config', { headers: auth() })
  if (!res.ok) return
  const cfg = await res.json()
  document.getElementById('name').value = cfg.name || ''
  document.getElementById('personality').value = cfg.personality || ''
  document.getElementById('voice').value = cfg.voice || ''
}

// ── Admin ─────────────────────────────────────────────────────────────────────

async function loadUsers() {
  const res = await fetch('/admin/keys', { headers: auth() })
  if (!res.ok) return
  const users = await res.json()
  const tbody = document.getElementById('userTable')
  tbody.innerHTML = users
    .map(
      (u) => `
    <tr id="row-${u.key}">
      <td>${u.name}</td>
      <td><span class="key-val">${u.key.slice(0, 12)}…</span></td>
      <td style="white-space:nowrap">
        <button class="sm" onclick="toggleEdit('${u.key}')">Edit</button>
        <button class="sm danger" onclick="deleteUser('${u.key}')">Del</button>
      </td>
    </tr>
    <tr id="edit-${u.key}" class="edit-row hidden">
      <td colspan="3">
        <div id="ef-${u.key}">loading...</div>
      </td>
    </tr>
  `
    )
    .join('')
}

async function toggleEdit(k) {
  const row = document.getElementById('edit-' + k)
  if (!row.classList.contains('hidden')) {
    row.classList.add('hidden')
    return
  }
  row.classList.remove('hidden')
  const div = document.getElementById('ef-' + k)
  if (div.dataset.loaded) return
  div.dataset.loaded = '1'

  const res = await fetch('/admin/keys/' + k, { headers: auth() })
  const cfg = await res.json()
  const pfx = 'u' + k + '_'

  div.innerHTML = `
    <label>Name</label>
    <input id="${pfx}name" value="${cfg.name || ''}">
    <div style="margin-top: 0.5rem">
      <button class="sm" style="background: var(--accent); color: #fff;" onclick="saveUser('${k}','${pfx}')">Save</button>
      <span class="flash" id="${pfx}flash">Saved</span>
    </div>
  `
}

async function saveUser(k, pfx) {
  const body = {
    name: document.getElementById(pfx + 'name').value.trim()
  }
  const res = await fetch('/admin/keys/' + k, { method: 'PUT', headers: jsonAuth(), body: JSON.stringify(body) })
  if (res.ok) {
    flash(pfx + 'flash')
    loadUsers()
  }
}

async function createUser() {
  const name = document.getElementById('newName').value.trim()
  if (!name) return
  const body = { name }
  const res = await fetch('/admin/keys', { method: 'POST', headers: jsonAuth(), body: JSON.stringify(body) })
  if (!res.ok) return
  const data = await res.json()
  document.getElementById('newKeyVal').textContent = data.key
  document.getElementById('newKeyBox').classList.remove('hidden')
  document.getElementById('newName').value = ''
  loadUsers()
}

async function deleteUser(k) {
  if (!confirm('Delete user ' + k.slice(0, 8) + '...?')) return
  await fetch('/admin/keys/' + k, { method: 'DELETE', headers: auth() })
  loadUsers()
}

init()
