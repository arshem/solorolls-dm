#pragma once

const char PORTAL_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>AI-Lite Setup</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; }
    body {
      font-family: system-ui, sans-serif;
      background: #0f0f0f;
      color: #e0e0e0;
      margin: 0;
      padding: 1rem;
    }
    h1 { font-size: 1.4rem; margin: 0 0 1rem; color: #7dd3fc; }
    fieldset {
      border: 1px solid #333;
      border-radius: 8px;
      padding: 1rem;
      margin-bottom: 1rem;
    }
    legend { color: #7dd3fc; font-weight: bold; padding: 0 .4rem; }
    label {
      display: block;
      margin-bottom: .75rem;
      font-size: .9rem;
      color: #aaa;
    }
    input, select {
      display: block;
      width: 100%;
      margin-top: .25rem;
      padding: .5rem .75rem;
      background: #1e1e1e;
      border: 1px solid #444;
      border-radius: 6px;
      color: #e0e0e0;
      font-size: 1rem;
    }
    input:focus, select:focus { outline: 2px solid #7dd3fc; border-color: transparent; }
    button {
      width: 100%;
      padding: .75rem;
      background: #0ea5e9;
      border: none;
      border-radius: 8px;
      color: #fff;
      font-size: 1rem;
      font-weight: bold;
      cursor: pointer;
    }
    button:hover { background: #38bdf8; }
    #ssid_custom { margin-top: .5rem; }
  </style>
</head>
<body>
  <h1>AI-Lite Setup</h1>
  <form onsubmit="save(event)">
    <fieldset>
      <legend>WiFi</legend>
      <label>Network
        <select id="ssid_sel" onchange="selChange()">
          <option value="">Custom...</option>
          {{WIFI_OPTIONS}}
        </select>
        <input id="ssid_custom" type="text" placeholder="Enter SSID" value="{{SSID}}" />
      </label>
      <label>Password
        <input id="password" type="password" value="{{PASS}}" autocomplete="current-password" />
      </label>
    </fieldset>
    <fieldset>
      <legend>Worker</legend>
      <label>Worker URL
        <input id="workerUrl" type="url" value="{{WORKER_URL}}" placeholder="https://your-worker.workers.dev" />
      </label>
      <label>API Key
        <input id="apiKey" type="text" value="{{API_KEY}}" placeholder="your-api-key" autocomplete="off" />
      </label>
    </fieldset>
    <button type="submit">Save &amp; Restart</button>
  </form>
  <script>
    function selChange() {
      const v = document.getElementById('ssid_sel').value;
      document.getElementById('ssid_custom').style.display = v ? 'none' : 'block';
    }
    selChange();

    async function save(e) {
      e.preventDefault();
      const sel = document.getElementById('ssid_sel').value;
      const body = JSON.stringify({
        ssid:      sel || document.getElementById('ssid_custom').value,
        password:  document.getElementById('password').value,
        workerUrl: document.getElementById('workerUrl').value,
        apiKey:    document.getElementById('apiKey').value
      });
      try {
        await fetch('/save', { method: 'POST', headers: {'Content-Type':'application/json'}, body });
        alert('Saved! Restarting...');
      } catch (err) {
        alert('Save failed: ' + err);
      }
    }
  </script>
</body>
</html>
)=====";
