const VOICES = {
  'aura-2-en': ['amalthea','andromeda','apollo','arcas','aries','asteria','athena','atlas','aurora','callista','cora','cordelia','delia','draco','electra','harmonia','helena','hera','hermes','hyperion','iris','janus','juno','jupiter','luna','mars','minerva','neptune','odysseus','ophelia','orion','orpheus','pandora','phoebe','pluto','saturn','thalia','theia','vesta','zeus'],
  'aura-2-es': ['sirio','nestor','carina','celeste','alvaro','diana','aquila','selena','estrella','javier']
}
const key = () => localStorage.getItem('apikey') || ''
const auth = () => ({ Authorization: 'Bearer ' + key() })
const jsonAuth = () => ({ ...auth(), 'Content-Type': 'application/json' })

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
}

function onModelChange(pfx) {
  const m = document.getElementById(pfx + 'ttsModel').value
  const voices = VOICES[m] || VOICES['aura-2-en']
  const sel = document.getElementById(pfx + 'ttsVoice')
  const prev = sel.value
  sel.innerHTML = voices.map((v) => `<option>${v}</option>`).join('')
  if (voices.includes(prev)) sel.value = prev
}

function applyCfg(cfg, pfx = '') {
  document.getElementById(pfx + 'name').value = cfg.name || ''
  document.getElementById(pfx + 'personality').value = cfg.personality || ''
  const model = VOICES[cfg.ttsModel] ? cfg.ttsModel : 'aura-2-en'
  document.getElementById(pfx + 'ttsModel').value = model
  onModelChange(pfx)
  const defaultVoice = model === 'aura-2-es' ? 'aquila' : 'luna'
  document.getElementById(pfx + 'ttsVoice').value = cfg.ttsVoice || defaultVoice
}

function readCfg(pfx = '') {
  return {
    name: document.getElementById(pfx + 'name').value.trim(),
    personality: document.getElementById(pfx + 'personality').value.trim(),
    ttsModel: document.getElementById(pfx + 'ttsModel').value,
    ttsVoice: document.getElementById(pfx + 'ttsVoice').value
  }
}

function logout() {
  localStorage.removeItem('apikey')
  document.getElementById('apikey').value = ''
  document.getElementById('whoami').textContent = ''
  document.getElementById('logoutBtn').classList.add('hidden')
  for (const id of ['chatSection', 'cfgSection', 'adminSection']) document.getElementById(id).classList.add('hidden')
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

  const [whoRes, cfgRes] = await Promise.all([fetch('/whoami', { headers: auth() }), fetch('/config', { headers: auth() })])

  if (!whoRes.ok) {
    document.getElementById('whoami').textContent = 'Invalid key'
    return
  }

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
    const res = await fetch('/chat', { method: 'POST', headers: jsonAuth(), body: JSON.stringify({ text }) })
    if (!res.ok) {
      addMsg('Error ' + res.status + ': ' + (await res.text()), 'err')
      return
    }
    const data = await res.json()
    addMsg(data.text, 'ai')
    document.getElementById('status').textContent = 'Playing...'
    const mp3 = Uint8Array.from(atob(data.audio), (c) => c.charCodeAt(0))
    const audio = new Audio(URL.createObjectURL(new Blob([mp3], { type: 'audio/mpeg' })))
    audio.onended = () => (document.getElementById('status').textContent = '')
    audio.play()
  } catch (e) {
    addMsg('Error: ' + e.message, 'err')
    document.getElementById('status').textContent = ''
  } finally {
    document.getElementById('sendBtn').disabled = false
  }
}

async function saveConfig() {
  const res = await fetch('/config', { method: 'POST', headers: jsonAuth(), body: JSON.stringify(readCfg()) })
  if (res.ok) flash('cfgFlash')
}

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
      <td>${u.ttsModel || 'melotts'}</td>
      <td><span class="key-val">${u.key}</span></td>
      <td style="white-space:nowrap">
        <button class="sm" onclick="toggleEdit('${u.key}')">Edit</button>
        <button class="sm danger" onclick="deleteUser('${u.key}')">Delete</button>
      </td>
    </tr>
    <tr id="edit-${u.key}" class="edit-row hidden">
      <td colspan="4">
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
    <label>Personality</label>
    <textarea id="${pfx}personality" rows="3"></textarea>
    <label>Language</label>
    <select id="${pfx}ttsModel" onchange="onModelChange('${pfx}')">
      <option value="aura-2-en">English</option>
      <option value="aura-2-es">Español</option>
    </select>
    <label>Voice</label>
    <select id="${pfx}ttsVoice"></select>
    <div class="row">
      <button class="primary sm" onclick="saveUser('${k}','${pfx}')">Save</button>
      <span class="flash" id="${pfx}flash">Saved</span>
    </div>
  `
  applyCfg(cfg, pfx)
}

async function saveUser(k, pfx) {
  const body = {
    name: document.getElementById(pfx + 'name').value.trim(),
    ...readCfg(pfx)
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
  const body = { name, ...readCfg('new_') }
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

// ── WAV encoder (16kHz mono PCM — format Whisper reliably handles) ────────────
function writeStr(view, offset, str) {
  for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i))
}

function encodePCM(samples) {
  const buf = new ArrayBuffer(44 + samples.length * 2)
  const v = new DataView(buf)
  const rate = 16000
  writeStr(v, 0, 'RIFF')
  v.setUint32(4, 36 + samples.length * 2, true)
  writeStr(v, 8, 'WAVE')
  writeStr(v, 12, 'fmt ')
  v.setUint32(16, 16, true)
  v.setUint16(20, 1, true) // PCM
  v.setUint16(22, 1, true)
  v.setUint32(24, rate, true) // mono, 16kHz
  v.setUint32(28, rate * 2, true)
  v.setUint16(32, 2, true)
  v.setUint16(34, 16, true)
  writeStr(v, 36, 'data')
  v.setUint32(40, samples.length * 2, true)
  for (let i = 0; i < samples.length; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]))
    v.setInt16(44 + i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true)
  }
  return buf
}

async function blobToWAV(blob) {
  console.log('[blobToWAV] blob size:', blob.size, 'type:', blob.type)
  const raw = await blob.arrayBuffer()
  const ctx = new AudioContext()
  await ctx.resume()
  const decoded = await ctx.decodeAudioData(raw)
  ctx.close()
  const ch0 = decoded.getChannelData(0)
  const maxRaw = ch0.reduce((m, v) => Math.max(m, Math.abs(v)), 0)
  console.log('[blobToWAV] decoded duration:', decoded.duration, 'sampleRate:', decoded.sampleRate, 'channels:', decoded.numberOfChannels, 'maxAmp:', maxRaw)

  // Resample to 16kHz mono
  const offline = new OfflineAudioContext(1, Math.ceil(decoded.duration * 16000), 16000)
  const src = offline.createBufferSource()
  src.buffer = decoded
  src.connect(offline.destination)
  src.start()
  const resampled = await offline.startRendering()
  const rch = resampled.getChannelData(0)
  const maxResampled = rch.reduce((m, v) => Math.max(m, Math.abs(v)), 0)
  console.log('[blobToWAV] resampled samples:', rch.length, 'maxAmp:', maxResampled)
  return encodePCM(rch)
}

// ── Voice recording ───────────────────────────────────────────────────────────
let mediaRecorder = null
let recChunks = []

async function toggleRec() {
  if (mediaRecorder && mediaRecorder.state === 'recording') {
    mediaRecorder.stop()
    return
  }

  let stream
  try {
    stream = await navigator.mediaDevices.getUserMedia({ audio: true })
  } catch (e) {
    document.getElementById('status').textContent = 'Mic access denied'
    return
  }

  recChunks = []
  mediaRecorder = new MediaRecorder(stream)
  mediaRecorder.ondataavailable = (e) => {
    if (e.data.size > 0) recChunks.push(e.data)
  }
  mediaRecorder.onstop = async () => {
    stream.getTracks().forEach((t) => t.stop())
    document.getElementById('recBtn').classList.remove('recording')
    document.getElementById('recBtn').textContent = '⏺'
    const blob = new Blob(recChunks, { type: mediaRecorder.mimeType })
    await sendAudio(blob)
  }
  mediaRecorder.start()
  document.getElementById('recBtn').classList.add('recording')
  document.getElementById('recBtn').textContent = '⏹'
  document.getElementById('status').textContent = 'Recording… click ⏹ to stop'
}

async function sendAudio(blob) {
  document.getElementById('sendBtn').disabled = true
  document.getElementById('status').textContent = 'Converting…'
  let wavBuffer
  try {
    wavBuffer = await blobToWAV(blob)
  } catch (e) {
    addMsg('Audio conversion failed: ' + e.message, 'err')
    document.getElementById('status').textContent = ''
    document.getElementById('sendBtn').disabled = false
    return
  }

  // Diagnostic: play converted WAV locally so you can verify it's not silent
  const wavUrl = URL.createObjectURL(new Blob([wavBuffer], { type: 'audio/wav' }))
  const preview = document.createElement('audio')
  preview.controls = true
  preview.src = wavUrl
  preview.style.cssText = 'width:100%;margin:.25rem 0;display:block'
  const wrapper = document.createElement('div')
  wrapper.className = 'msg ai'
  wrapper.style.maxWidth = '100%'
  wrapper.appendChild(Object.assign(document.createElement('div'), { textContent: '🎙 WAV preview (' + Math.round(wavBuffer.byteLength / 1024) + 'kB) — verify audio then wait for response:', style: 'font-size:.75rem;color:#888;margin-bottom:.25rem' }))
  wrapper.appendChild(preview)
  const chat = document.getElementById('chat')
  chat.appendChild(wrapper)
  chat.scrollTop = chat.scrollHeight

  document.getElementById('status').textContent = 'Processing…'
  try {
    const res = await fetch('/chat', {
      method: 'POST',
      headers: { ...auth(), Accept: 'application/json', 'Content-Type': 'audio/wav' },
      body: wavBuffer
    })
    if (!res.ok) {
      addMsg('Error ' + res.status + ': ' + (await res.text()), 'err')
      return
    }
    const data = await res.json()
    addMsg(data.input, 'user')
    addMsg(data.text, 'ai')
    document.getElementById('status').textContent = 'Playing…'
    const mp3 = Uint8Array.from(atob(data.audio), (c) => c.charCodeAt(0))
    const audio = new Audio(URL.createObjectURL(new Blob([mp3], { type: 'audio/mpeg' })))
    audio.onended = () => (document.getElementById('status').textContent = '')
    audio.play()
  } catch (e) {
    addMsg('Error: ' + e.message, 'err')
    document.getElementById('status').textContent = ''
  } finally {
    document.getElementById('sendBtn').disabled = false
  }
}

onModelChange('new_')
init()
