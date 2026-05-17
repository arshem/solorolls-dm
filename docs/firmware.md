# Firmware Guide

The firmware runs on the AiPi Lite (ESP32-S3) and handles audio capture, playback, display, WiFi, and WebSocket communication with the server.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 with 8MB PSRAM |
| Audio codec | ES8311 (I2C control + I2S audio) |
| Display | ST7735 128x128 LCD (SPI) |
| Buttons | A (GPIO 1, RTC-capable), B (GPIO 42) |
| Speaker enable | GPIO 9 |
| Battery ADC | GPIO 2 (voltage divider, ~2x) |
| Power control | GPIO 10 (held during deep sleep) |

Pin definitions are in `firmware/src/pins.h`.

## Build and Flash

Compile with PlatformIO:

```bash
cd firmware
pio run
```

Flash the resulting binary with esptool:

```bash
esptool --chip esp32s3 write-flash 0x0 firmware/.pio/build/ailite/firmware.factory.bin
```

Erase all saved settings:

```bash
esptool --chip esp32s3 erase-flash
```

## Dependencies

Managed by PlatformIO (`platformio.ini`):

| Library | Purpose |
|---------|---------|
| [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) | Audio framework |
| [arduino-audio-driver](https://github.com/pschatzmann/arduino-audio-driver) | ES8311 codec driver |
| [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) | ST7735 display driver |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | (available but portal is custom) |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | JSON parsing |
| [WebSockets](https://github.com/links2004/WebSockets) | WebSocket client |

## Boot Flow

1. Power on → initialize display, PSRAM, I2C pins, load settings from NVS
2. If **B held at boot** → open captive portal → sleep after timeout or save
3. If **no WiFi/server credentials** → show error → open portal → sleep
4. Connect WiFi (tries active network first with cached IP, then cycles through saved networks)
5. Cache DHCP-assigned IP for faster reconnection next boot
6. Fetch assistant name from server (`GET /config`)
7. Connect WebSocket to server
8. Start face animation → show "Ready, press A: begin"
9. **A pressed** → start continuous mic streaming
10. Main loop: handle settings menu, config portal shortcut, idle timeout

## Audio Pipeline

### Recording (mic → server)

- Hardware captures at **24kHz stereo 16-bit** via ES8311 I2S
- Firmware downsamples to **16kHz mono** using 3:2 linear interpolation
- Sends raw PCM as WebSocket binary frames continuously
- Gemini's built-in VAD handles turn-taking — no explicit "done" signal

The mic task runs on **Core 1** with priority 2.

### Playback (server → speaker)

- Server sends **24kHz mono 16-bit PCM** as binary WebSocket frames
- Firmware buffers in a **96KB ring buffer** (PSRAM)
- Playback starts after **6KB prebuffer** (~125ms of audio)
- Upsamples 24kHz mono → **48kHz stereo** (duplicate each sample 2x, copy to both channels)
- Outputs via ES8311 I2S

The playback task runs on **Core 1** with priority 3.

### Volume Control

Volume is applied via the ES8311 codec's built-in gain control. Mic gain is applied via the codec's input gain. Both are adjustable from the settings menu.

## Multi-WiFi

The device stores up to **4 WiFi networks** in NVS. On boot:
1. Tries the active network first (with cached IP for fast DHCP skip)
2. If that fails, clears cached IP and tries each saved network in order
3. First successful connection becomes the new active network

The settings menu WiFi screen lets you switch between saved networks or add new ones (which opens the captive portal).

## Captive Portal

- Creates AP named **"SoloRolls-DM"** (open, no password)
- Runs a DNS server (redirects all domains to 192.168.4.1)
- Serves an HTML config page (stored in `firmware/src/portal.h`)
- Accepts `POST /save` with JSON: `{ssid, password, workerUrl, apiKey}`
- Accepts `POST /remove` with JSON: `{index}` to delete a saved network
- Shows available WiFi networks (scanned) and saved networks
- Times out after **5 minutes** and restarts

## NVS Storage

Namespace: `"ailite"`

| Key | Type | Description |
|-----|------|-------------|
| `workerUrl` | string | Server URL |
| `apiKey` | string | User API key |
| `assistantName` | string | DM display name (fetched from server) |
| `wifiCount` | int | Number of saved networks |
| `wSSID0`–`wSSID3` | string | WiFi SSIDs |
| `wPASS0`–`wPASS3` | string | WiFi passwords |
| `wifiActive` | int | Index of active network |
| `volIndex` | int | Volume level index (0–4) |
| `micIndex` | int | Mic gain index (0–4) |
| `sleepIdx` | int | Sleep timer index (0–3) |
| `brightIdx` | int | Brightness index (0–4) |
| `ip`, `gw`, `sn`, `dns` | uint32 | Cached DHCP addresses |

## Animated Face

A wizard face displays on the LCD with states driven by WebSocket events:

| State | Trigger | Animation |
|-------|---------|-----------|
| IDLE | Default after connection | Random blinks every 2–5s |
| LISTEN | `transcript` message received | Eyes wide open |
| THINK | (transitional) | Eyes shift side to side |
| SPEAK | `response` message received | Mouth animates (4 frames) |
| SLEEP | Deep sleep entry | Eyes closed |

The face task runs on **Core 1** at ~10fps.

## WebSocket Events

| Event | Action |
|-------|--------|
| Binary frame | Write to ring buffer → triggers playback |
| `{"type":"transcript"}` | Set face to LISTEN |
| `{"type":"response"}` | Set face to SPEAK |
| `{"type":"interrupted"}` | Stop playback, set face to LISTEN |
| `{"type":"error"}` | Stop face, show error on display |

Connection uses heartbeat: ping every 15s, 5s pong timeout, drop after 2 missed pongs. Auto-reconnect interval: 5s.

## Deep Sleep

- Triggered by idle timeout (configurable: 2m, 5m, 10m, or never)
- Wake source: Button A (GPIO 1, ext0 wakeup on LOW)
- Power rail held active via `rtc_gpio_hold_en(PIN_PWR_CTL)`
- Display backlight turned off, speaker disabled

## Settings Menu

Accessed by holding A + tapping B during a session. Mic streaming pauses while the menu is open.

Navigation:
- **B** — scroll to next item
- **A** — select/enter submenu
- **A+B held** — go back / exit menu

All settings are saved to NVS when exiting the main menu.
