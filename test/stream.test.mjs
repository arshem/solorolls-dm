import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, writeFileSync } from 'node:fs'

// const BASE = 'ws://localhost:8787'
const BASE = 'wss://ailite.konsumer.workers.dev'

const { API_SECRET, API_KEY } = process.env
const KEY = API_KEY || API_SECRET
const CHUNK_SIZE = 4096
const TIMEOUT_MS = 30_000
const audio = readFileSync(new URL('test.wav', import.meta.url))

function runPipeline(key, audioBuffer) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${BASE}/ws?key=${key}`)
    ws.binaryType = 'arraybuffer'

    const result = { transcript: null, response: null, audioBytes: 0, audioFrames: 0, audioChunks: [] }
    const timer = setTimeout(() => { ws.close(); reject(new Error('timeout')) }, TIMEOUT_MS)

    ws.addEventListener('open', () => {
      let offset = 0
      while (offset < audioBuffer.length) {
        ws.send(audioBuffer.slice(offset, Math.min(offset + CHUNK_SIZE, audioBuffer.length)))
        offset += CHUNK_SIZE
      }
      ws.send(JSON.stringify({ type: 'done' }))
    })

    ws.addEventListener('message', (event) => {
      if (event.data instanceof ArrayBuffer) {
        result.audioChunks.push(Buffer.from(event.data))
        result.audioBytes += event.data.byteLength
        result.audioFrames++
        return
      }
      const msg = JSON.parse(event.data)
      if (msg.type === 'transcript') result.transcript = msg.text
      if (msg.type === 'response') result.response = msg.text
      if (msg.type === 'audio_end') { clearTimeout(timer); ws.close(); resolve(result) }
      if (msg.type === 'error') { clearTimeout(timer); ws.close(); reject(new Error(msg.message)) }
    })

    ws.addEventListener('error', (e) => { clearTimeout(timer); reject(new Error(e.message || 'ws error')) })
  })
}

test('rejects missing key', async () => {
  const ws = new WebSocket(`${BASE}/ws`)
  const code = await new Promise((resolve) => ws.addEventListener('close', (e) => resolve(e.code)))
  // Cloudflare returns HTTP 401 before upgrade — close code 1006 (abnormal) or 4001
  assert.ok(code !== 1000, `expected non-clean close, got ${code}`)
})

test('full pipeline: STT → LLM → streaming TTS', { timeout: TIMEOUT_MS }, async () => {
  assert.ok(KEY, 'API_KEY or API_SECRET env var required')
  const result = await runPipeline(KEY, audio)

  assert.ok(result.transcript?.length > 0, 'transcript should be non-empty')
  assert.ok(result.response?.length > 0, 'LLM response should be non-empty')
  assert.ok(result.audioFrames >= 1, `TTS should stream at least one frame, got ${result.audioFrames}`)
  assert.ok(result.audioBytes > 1024, `audio should be >1kB, got ${result.audioBytes}B`)

  const outPath = new URL('out.mp3', import.meta.url).pathname
  writeFileSync(outPath, Buffer.concat(result.audioChunks))
  console.log(`  saved → ${outPath}  (transcript: "${result.transcript}")`)
})

test('second turn on same connection', { timeout: TIMEOUT_MS * 2 }, async () => {
  assert.ok(KEY, 'API_KEY or API_SECRET env var required')

  const ws = new WebSocket(`${BASE}/ws?key=${KEY}`)
  ws.binaryType = 'arraybuffer'

  async function turn() {
    return new Promise((resolve, reject) => {
      const result = { transcript: null, response: null, audioBytes: 0 }
      const timer = setTimeout(() => reject(new Error('turn timeout')), TIMEOUT_MS)

      const onMessage = (event) => {
        if (event.data instanceof ArrayBuffer) { result.audioBytes += event.data.byteLength; return }
        const msg = JSON.parse(event.data)
        if (msg.type === 'transcript') result.transcript = msg.text
        if (msg.type === 'response') result.response = msg.text
        if (msg.type === 'audio_end') { clearTimeout(timer); ws.removeEventListener('message', onMessage); resolve(result) }
        if (msg.type === 'error') { clearTimeout(timer); ws.removeEventListener('message', onMessage); reject(new Error(msg.message)) }
      }

      ws.addEventListener('message', onMessage)
      ws.addEventListener('error', (e) => { clearTimeout(timer); reject(new Error(e.message || 'ws error')) })

      let offset = 0
      while (offset < audio.length) {
        ws.send(audio.slice(offset, Math.min(offset + CHUNK_SIZE, audio.length)))
        offset += CHUNK_SIZE
      }
      ws.send(JSON.stringify({ type: 'done' }))
    })
  }

  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve)
    ws.addEventListener('error', reject)
  })

  const t1 = await turn()
  const t2 = await turn()
  ws.close()

  assert.ok(t1.transcript?.length > 0, 'turn 1 transcript empty')
  assert.ok(t2.transcript?.length > 0, 'turn 2 transcript empty')
  assert.ok(t2.audioBytes > 1024, 'turn 2 audio missing')
})
