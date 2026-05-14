import { test, before, after } from 'node:test'
import assert from 'node:assert/strict'

// const BASE = 'http://localhost:8787'
const BASE = 'https://ailite.konsumer.workers.dev'

const { API_SECRET, API_KEY } = process.env
const KEY = API_KEY || API_SECRET

async function api(path, key, method = 'GET', body) {
  const opts = { method, headers: {} }
  if (key) opts.headers['Authorization'] = `Bearer ${key}`
  if (body !== undefined) {
    opts.body = JSON.stringify(body)
    opts.headers['Content-Type'] = 'application/json'
  }
  const r = await fetch(`${BASE}${path}`, opts)
  const text = await r.text()
  let json
  try { json = JSON.parse(text) } catch {}
  return { status: r.status, body: json ?? text }
}

// Test user created before suite, deleted after.
let userKey

before(async () => {
  const { status, body } = await api('/admin/keys', API_SECRET, 'POST', { name: 'Test User' })
  assert.equal(status, 200, `setup: create user failed: ${JSON.stringify(body)}`)
  userKey = body.key
})

after(async () => {
  await api(`/admin/keys/${userKey}`, API_SECRET, 'DELETE')
})

// ── Auth ──────────────────────────────────────────────────────────────────────

test('no auth → 401', async () => {
  const { status } = await api('/whoami')
  assert.equal(status, 401)
})

test('bad token → 401', async () => {
  const { status } = await api('/whoami', 'not-a-real-key')
  assert.equal(status, 401)
})

// ── whoami ────────────────────────────────────────────────────────────────────

test('whoami: admin key → isAdmin true', async () => {
  const { status, body } = await api('/whoami', API_SECRET)
  assert.equal(status, 200)
  assert.equal(body.isAdmin, true)
  assert.equal(body.name, 'Admin')
})

test('whoami: user key → isAdmin false', async () => {
  const { status, body } = await api('/whoami', userKey)
  assert.equal(status, 200)
  assert.equal(body.isAdmin, false)
  assert.equal(body.name, 'Test User')
})

test('whoami: API_KEY is valid user', { skip: !API_KEY }, async () => {
  const { status, body } = await api('/whoami', API_KEY)
  assert.equal(status, 200)
  assert.equal(body.isAdmin, false)
  assert.ok(body.name?.length > 0, 'API_KEY user has no name')
})

// ── config ────────────────────────────────────────────────────────────────────

test('config: GET returns user fields', async () => {
  const { status, body } = await api('/config', userKey)
  assert.equal(status, 200)
  assert.ok(body.name)
  assert.ok(body.ttsModel)
  assert.ok(body.ttsVoice)
})

test('config: API_KEY is valid user', async () => {
  const { status, body } = await api('/config', API_KEY)
  assert.equal(status, 200)
  assert.ok(body.name)
  assert.ok(body.ttsModel)
  assert.ok(body.ttsVoice)
})

test('config: POST updates fields', async () => {
  const { status } = await api('/config', userKey, 'POST', { name: 'Renamed' })
  assert.equal(status, 200)
  const { body } = await api('/config', userKey)
  assert.equal(body.name, 'Renamed')
})

test('config: admin POST → 400 (no personal config)', async () => {
  const { status } = await api('/config', API_SECRET, 'POST', { name: 'x' })
  assert.equal(status, 400)
})

// ── admin/keys ────────────────────────────────────────────────────────────────

test('admin/keys: GET lists users', async () => {
  const { status, body } = await api('/admin/keys', API_SECRET)
  assert.equal(status, 200)
  assert.ok(Array.isArray(body))
  assert.ok(body.some((u) => u.key === userKey))
})

test('admin/keys: GET single user', async () => {
  const { status, body } = await api(`/admin/keys/${userKey}`, API_SECRET)
  assert.equal(status, 200)
  assert.equal(body.key, userKey)
})

test('admin/keys: PUT updates user', async () => {
  const { status } = await api(`/admin/keys/${userKey}`, API_SECRET, 'PUT', { name: 'Admin Renamed', ttsVoice: 'asteria' })
  assert.equal(status, 200)
  const { body } = await api(`/admin/keys/${userKey}`, API_SECRET)
  assert.equal(body.name, 'Admin Renamed')
  assert.equal(body.ttsVoice, 'asteria')
})

test('admin/keys: user key → 403', async () => {
  const { status } = await api('/admin/keys', userKey)
  assert.equal(status, 403)
})

test('admin/keys: GET unknown key → 404', async () => {
  const { status } = await api('/admin/keys/doesnotexist', API_SECRET)
  assert.equal(status, 404)
})

// ── chat ──────────────────────────────────────────────────────────────────────

test('chat: text mode returns text + audio', async () => {
  const { status, body } = await api('/chat', userKey, 'POST', { text: 'say the word yes' })
  assert.equal(status, 200)
  assert.ok(body.text)
  assert.ok(body.audio)
})

test('chat: missing text → 400', async () => {
  const { status } = await api('/chat', userKey, 'POST', {})
  assert.equal(status, 400)
})

test('chat: no auth → 401', async () => {
  const { status } = await api('/chat', null, 'POST', { text: 'hello' })
  assert.equal(status, 401)
})
