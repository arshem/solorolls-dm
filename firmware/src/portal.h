#pragma once

const char PORTAL_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SoloRolls DM Setup</title>
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
      margin-top: .5rem;
    }
    button:hover { background: #38bdf8; }
    .btn-danger { background: #dc2626; }
    .btn-danger:hover { background: #ef4444; }
    .btn-secondary { background: #374151; }
    .btn-secondary:hover { background: #4b5563; }
    #ssid_custom { margin-top: .5rem; }
    .saved-net {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: .5rem .75rem;
      background: #1e1e1e;
      border: 1px solid #333;
      border-radius: 6px;
      margin-bottom: .5rem;
    }
    .saved-net .name { font-size: .95rem; }
    .saved-net .active { color: #4ade80; font-size: .8rem; margin-left: .5rem; }
    .saved-net button {
      width: auto;
      padding: .3rem .6rem;
      font-size: .8rem;
      margin: 0;
    }
    .empty-msg { color: #666; font-style: italic; font-size: .9rem; }
  </style>
</head>
<body>
  <h1>SoloRolls DM Setup</h1>

  <fieldset>
    <legend>Saved Networks</legend>
    <div id="saved_nets">{{SAVED_NETS}}</div>
  </fieldset>

  <form onsubmit="save(event)">
    <fieldset>
      <legend>Add WiFi Network</legend>
      <label>Network
        <select id="ssid_sel" onchange="selChange()">
          <option value="">Custom...</option>
          {{WIFI_OPTIONS}}
        </select>
        <input id="ssid_custom" type="text" placeholder="Enter SSID" value="" />
      </label>
      <label>Password
        <input id="password" type="password" value="" autocomplete="current-password" />
      </label>
      <button type="submit">Add Network</button>
    </fieldset>

    <fieldset>
      <legend>Server</legend>
      <label>Server URL
        <input id="workerUrl" type="url" value="{{WORKER_URL}}" placeholder="https://<yourdomain>" />
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
      const custom = document.getElementById('ssid_custom');
      custom.style.display = v ? 'none' : 'block';
      if (v) custom.value = '';
    }
    selChange();

    async function save(e) {
      e.preventDefault();
      const sel = document.getElementById('ssid_sel').value;
      const ssid = sel || document.getElementById('ssid_custom').value;
      if (!ssid) { alert('Please select or enter a WiFi network'); return; }
      const body = JSON.stringify({
        ssid:      ssid,
        password:  document.getElementById('password').value,
        workerUrl: document.getElementById('workerUrl').value,
        apiKey:    document.getElementById('apiKey').value
      });
      try {
        const r = await fetch('/save', { method: 'POST', headers: {'Content-Type':'application/json'}, body });
        if (r.ok) alert('Saved! Restarting...');
        else alert('Error: ' + await r.text());
      } catch (err) {
        alert('Save failed: ' + err);
      }
    }

    async function removeNet(idx) {
      if (!confirm('Remove this network?')) return;
      try {
        const r = await fetch('/remove', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({index: idx}) });
        if (r.ok) location.reload();
        else alert('Error: ' + await r.text());
      } catch (err) {
        alert('Remove failed: ' + err);
      }
    }
  </script>
</body>
</html>)=====";
