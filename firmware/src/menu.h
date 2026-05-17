#pragma once

// ── Settings Menu System ──────────────────────────────────────────────────────
// Hold A + tap B → open menu
// Tap B → scroll, Tap A → select
// Hold A+B → back out

enum MenuItem { MENU_NEW_GAME, MENU_VOLUME, MENU_MIC, MENU_SLEEP, MENU_DISPLAY, MENU_WIFI, MENU_ABOUT, MENU_COUNT };
static const char *MENU_LABELS[] = {"New Game", "Volume", "Mic Gain", "Sleep Timer", "Display", "WiFi", "About"};

static volatile bool g_inMenu = false;
static bool g_needReconnect = false;

// Helper: wait for both buttons released
void waitBothReleased() {
  while (digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW) delay(10);
  delay(50);
}

// Helper: wait for a single button released
void waitBtnRelease(int pin) {
  while (digitalRead(pin) == LOW) delay(10);
  delay(50);
}

// Check if both buttons held (for back/exit)
bool bothHeld() {
  return (digitalRead(PIN_BTN_A) == LOW && digitalRead(PIN_BTN_B) == LOW);
}

// Draw the main menu with scrolling support
#define MENU_VISIBLE  6   // max items visible at once
#define MENU_ITEM_H   16  // height per item in pixels
#define MENU_TOP_Y    16  // first item Y position (below header)
#define MENU_AREA_H   (MENU_VISIBLE * MENU_ITEM_H) // 96px

void drawMenu(int cursor) {
  gfx->fillRect(0, 0, 128, 128, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, 2);
  gfx->print("= Settings =");
  gfx->drawFastHLine(0, 12, 128, RGB565_WHITE);

  // Calculate scroll offset to keep cursor visible
  static int scrollTop = 0;
  if (cursor < scrollTop) scrollTop = cursor;
  if (cursor >= scrollTop + MENU_VISIBLE) scrollTop = cursor - MENU_VISIBLE + 1;

  // Draw visible items
  for (int vi = 0; vi < MENU_VISIBLE; vi++) {
    int i = scrollTop + vi;
    if (i >= MENU_COUNT) break;
    int y = MENU_TOP_Y + vi * MENU_ITEM_H;
    if (i == cursor) {
      gfx->fillRect(0, y, 124, 14, gfx->color565(40, 40, 80));
      gfx->setTextColor(RGB565_GREEN);
    } else {
      gfx->setTextColor(RGB565_WHITE);
    }
    gfx->setCursor(8, y + 3);
    gfx->print(MENU_LABELS[i]);
  }

  // Scrollbar (right edge) — only show if items exceed visible area
  if (MENU_COUNT > MENU_VISIBLE) {
    int sbX = 126;
    int sbH = MENU_AREA_H;
    int thumbH = max(8, sbH * MENU_VISIBLE / MENU_COUNT);
    int thumbY = MENU_TOP_Y + (sbH - thumbH) * scrollTop / (MENU_COUNT - MENU_VISIBLE);
    gfx->drawFastVLine(sbX, MENU_TOP_Y, sbH, gfx->color565(40, 40, 40));
    gfx->fillRect(sbX - 1, thumbY, 3, thumbH, gfx->color565(100, 100, 100));
  }

  // Footer hint
  gfx->setTextColor(gfx->color565(128, 128, 128));
  gfx->setCursor(4, 118);
  gfx->print("A:sel B:next A+B:back");
}

// Draw a slider-style submenu
void drawSlider(const char *title, int value, int maxVal, const char *label) {
  gfx->fillRect(0, 0, 128, 128, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, 4);
  gfx->print(title);
  gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);

  // Bar
  gfx->drawRect(8, 50, 112, 16, RGB565_WHITE);
  int barW = (int)(108.0f * value / maxVal);
  gfx->fillRect(10, 52, barW, 12, RGB565_GREEN);

  // Label
  gfx->setCursor(40, 75);
  gfx->setTextColor(RGB565_WHITE);
  gfx->print(label);

  gfx->setTextColor(gfx->color565(128, 128, 128));
  gfx->setCursor(4, 118);
  gfx->print("B:change  A+B:back");
}

// ── Submenu: Volume ───────────────────────────────────────────────────────────
void menuVolume() {
  while (true) {
    char lbl[16];
    snprintf(lbl, sizeof(lbl), "%d%%", (int)(VOL_LEVELS[g_volIndex] * 100));
    drawSlider("Volume", g_volIndex, VOL_COUNT - 1, lbl);

    while (true) {
      if (bothHeld()) { waitBothReleased(); return; }
      if (digitalRead(PIN_BTN_B) == LOW) {
        delay(50);
        if (!bothHeld()) {
          g_volIndex = (g_volIndex + 1) % VOL_COUNT;
          if (g_playing) i2s.setVolume(VOL_LEVELS[g_volIndex]);
          waitBtnRelease(PIN_BTN_B);
          break; // redraw
        }
      }
      delay(20);
    }
  }
}

// ── Submenu: Mic Sensitivity ──────────────────────────────────────────────────
void menuMic() {
  while (true) {
    char lbl[16];
    snprintf(lbl, sizeof(lbl), "%d%%", (int)(MIC_LEVELS[g_micIndex] * 100));
    drawSlider("Mic Gain", g_micIndex, MIC_COUNT - 1, lbl);

    while (true) {
      if (bothHeld()) { waitBothReleased(); return; }
      if (digitalRead(PIN_BTN_B) == LOW) {
        delay(50);
        if (!bothHeld()) {
          g_micIndex = (g_micIndex + 1) % MIC_COUNT;
          waitBtnRelease(PIN_BTN_B);
          break;
        }
      }
      delay(20);
    }
  }
}

// ── Submenu: Sleep Timer ──────────────────────────────────────────────────────
void menuSleep() {
  while (true) {
    gfx->fillRect(0, 0, 128, 128, RGB565_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(4, 4);
    gfx->print("Sleep Timer");
    gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);

    for (int i = 0; i < SLEEP_COUNT; i++) {
      int y = 24 + i * 18;
      if (i == g_sleepIndex) {
        gfx->fillRect(0, y, 128, 16, gfx->color565(40, 40, 80));
        gfx->setTextColor(RGB565_GREEN);
      } else {
        gfx->setTextColor(RGB565_WHITE);
      }
      gfx->setCursor(12, y + 4);
      gfx->print(SLEEP_LABELS[i]);
    }

    gfx->setTextColor(gfx->color565(128, 128, 128));
    gfx->setCursor(4, 118);
    gfx->print("B:change  A+B:back");

    while (true) {
      if (bothHeld()) { waitBothReleased(); return; }
      if (digitalRead(PIN_BTN_B) == LOW) {
        delay(50);
        if (!bothHeld()) {
          g_sleepIndex = (g_sleepIndex + 1) % SLEEP_COUNT;
          waitBtnRelease(PIN_BTN_B);
          break;
        }
      }
      delay(20);
    }
  }
}

// ── Submenu: Display Brightness ───────────────────────────────────────────────
void menuDisplay() {
  while (true) {
    int pct = (BRIGHT_LEVELS[g_brightIndex] * 100) / 255;
    char lbl[16];
    snprintf(lbl, sizeof(lbl), "%d%%", pct);
    drawSlider("Brightness", g_brightIndex, BRIGHT_COUNT - 1, lbl);

    while (true) {
      if (bothHeld()) { waitBothReleased(); return; }
      if (digitalRead(PIN_BTN_B) == LOW) {
        delay(50);
        if (!bothHeld()) {
          g_brightIndex = (g_brightIndex + 1) % BRIGHT_COUNT;
          analogWrite(PIN_LCD_BL, BRIGHT_LEVELS[g_brightIndex]);
          waitBtnRelease(PIN_BTN_B);
          break;
        }
      }
      delay(20);
    }
  }
}

// ── Submenu: WiFi ─────────────────────────────────────────────────────────────
void menuWifi() {
  int cursor = 0;
  while (true) {
    gfx->fillRect(0, 0, 128, 128, RGB565_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(4, 4);
    gfx->print("WiFi Networks");
    gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);

    int totalItems = g_wifiCount + 1; // networks + "Add new..."
    for (int i = 0; i < totalItems && i < MAX_WIFI_NETS + 1; i++) {
      int y = 18 + i * 16;
      if (i == cursor) {
        gfx->fillRect(0, y, 128, 14, gfx->color565(40, 40, 80));
        gfx->setTextColor(RGB565_GREEN);
      } else {
        gfx->setTextColor(RGB565_WHITE);
      }
      gfx->setCursor(8, y + 3);
      if (i < g_wifiCount) {
        gfx->print(g_wifiNets[i].ssid);
        if (i == g_wifiActive) gfx->print(" *");
      } else {
        gfx->print("+ Add new...");
      }
    }

    gfx->setTextColor(gfx->color565(128, 128, 128));
    gfx->setCursor(4, 118);
    gfx->print("A:sel B:next A+B:back");

    while (true) {
      if (bothHeld()) { waitBothReleased(); return; }
      if (digitalRead(PIN_BTN_B) == LOW) {
        delay(50);
        if (!bothHeld()) {
          cursor = (cursor + 1) % totalItems;
          waitBtnRelease(PIN_BTN_B);
          break;
        }
      }
      if (digitalRead(PIN_BTN_A) == LOW) {
        delay(50);
        if (!bothHeld() && digitalRead(PIN_BTN_A) == LOW) {
          waitBtnRelease(PIN_BTN_A);
          if (cursor < g_wifiCount) {
            // Switch to this network — will reconnect on menu exit
            g_wifiActive = cursor;
            // Show confirmation
            gfx->fillRect(0, 20, 128, 90, RGB565_BLACK);
            gfx->setTextColor(RGB565_GREEN);
            gfx->setCursor(8, 50);
            gfx->print("Selected:");
            gfx->setCursor(8, 65);
            gfx->print(g_wifiNets[cursor].ssid);
            delay(1000);
          } else {
            // "Add new" — open config portal
            waitBothReleased();
            return; // caller will handle portal launch
          }
          break;
        }
      }
      delay(20);
    }
  }
}

// ── Submenu: About ────────────────────────────────────────────────────────────
void menuAbout() {
  while (true) {
    gfx->fillRect(0, 0, 128, 128, RGB565_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(4, 4);
    gfx->print("About");
    gfx->drawFastHLine(0, 14, 128, RGB565_WHITE);

    int y = 20;
    gfx->setCursor(4, y); gfx->print("FW: "); gfx->print(FW_VERSION);
    y += 14;
    gfx->setCursor(4, y); gfx->print("WiFi: ");
    if (g_wifiActive >= 0 && g_wifiActive < g_wifiCount) {
      gfx->print(g_wifiNets[g_wifiActive].ssid);
    } else {
      gfx->print("--");
    }
    y += 14;
    gfx->setCursor(4, y); gfx->print("RSSI: ");
    gfx->print(WiFi.RSSI()); gfx->print(" dBm");
    y += 14;
    gfx->setCursor(4, y); gfx->print("IP: ");
    gfx->print(WiFi.localIP().toString());
    y += 14;
    gfx->setCursor(4, y); gfx->print("WS: ");
    gfx->print(g_wsConnected ? "Connected" : "Disconnected");
    y += 14;
    // Battery voltage (rough estimate from ADC)
    int rawAdc = analogRead(PIN_BAT_ADC);
    float voltage = rawAdc * 3.3f * 2.0f / 4095.0f; // assuming voltage divider
    gfx->setCursor(4, y); gfx->print("Bat: ");
    char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "%.1fV", voltage);
    gfx->print(vbuf);

    gfx->setTextColor(gfx->color565(128, 128, 128));
    gfx->setCursor(4, 118);
    gfx->print("A+B: back");

    // Wait for exit
    while (true) {
      if (bothHeld()) { waitBothReleased(); return; }
      delay(20);
    }
  }
}

// ── New Game ──────────────────────────────────────────────────────────────────
// Hits /reset to clear session history and force a fresh Gemini connection.
void menuNewGame() {
  showMsg("New Game?", "A:yes  B:no", RGB565_YELLOW);
  while (true) {
    if (bothHeld()) { waitBothReleased(); return; }
    if (digitalRead(PIN_BTN_A) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_A) == LOW) {
        waitBtnRelease(PIN_BTN_A);
        showMsg("Resetting...");
        String url = String(workerUrl);
        while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
        url += "/reset";
        HTTPClient http;
        http.begin(url);
        http.addHeader("Authorization", String("Bearer ") + apiKey);
        int code = http.POST("");
        http.end();
        if (code == 200) {
          showMsg("New Game", "ready!", RGB565_GREEN);
          g_needReconnect = true;
        } else {
          char buf[32];
          snprintf(buf, sizeof(buf), "err %d", code);
          showMsg("Reset fail", buf, RGB565_RED);
        }
        delay(1500);
        return;
      }
    }
    if (digitalRead(PIN_BTN_B) == LOW) {
      delay(50);
      if (digitalRead(PIN_BTN_B) == LOW) {
        waitBtnRelease(PIN_BTN_B);
        return;
      }
    }
    delay(20);
  }
}

// ── Main menu handler ─────────────────────────────────────────────────────────
// Returns true if WiFi portal should be opened after menu exits
bool openSettingsMenu() {
  g_inMenu = true;
  int cursor = 0;
  bool needPortal = false;

  drawMenu(cursor);

  while (true) {
    // Exit: hold both
    if (bothHeld()) {
      waitBothReleased();
      break;
    }

    // B tap: move cursor
    if (digitalRead(PIN_BTN_B) == LOW) {
      delay(50);
      if (!bothHeld()) {
        cursor = (cursor + 1) % MENU_COUNT;
        drawMenu(cursor);
        waitBtnRelease(PIN_BTN_B);
      }
    }

    // A tap: select item
    if (digitalRead(PIN_BTN_A) == LOW) {
      delay(50);
      if (!bothHeld() && digitalRead(PIN_BTN_A) == LOW) {
        waitBtnRelease(PIN_BTN_A);
        switch ((MenuItem)cursor) {
          case MENU_NEW_GAME: menuNewGame(); break;
          case MENU_VOLUME:  menuVolume();  break;
          case MENU_MIC:     menuMic();     break;
          case MENU_SLEEP:   menuSleep();   break;
          case MENU_DISPLAY: menuDisplay(); break;
          case MENU_WIFI:    menuWifi();    break;
          case MENU_ABOUT:   menuAbout();   break;
          default: break;
        }
        drawMenu(cursor); // redraw main menu after submenu exit
      }
    }

    delay(20);
  }

  // Save settings to NVS
  {
    Preferences prefs;
    prefs.begin("ailite", false);
    prefs.putInt("volIndex", g_volIndex);
    prefs.putInt("micIndex", g_micIndex);
    prefs.putInt("sleepIdx", g_sleepIndex);
    prefs.putInt("brightIdx", g_brightIndex);
    prefs.putInt("wifiActive", g_wifiActive);
    prefs.end();
  }

  g_inMenu = false;
  return needPortal;
}
