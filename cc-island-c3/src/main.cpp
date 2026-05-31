#include <Arduino.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "wifi_config.h"

namespace {
constexpr int kTftSclk = 4;
constexpr int kTftMosi = 6;
constexpr int kTftCs = 7;
constexpr int kTftDc = 2;
constexpr int kTftRst = 3;
constexpr int kBacklight = 10;

SPIClass spi(FSPI);
Adafruit_ST7789 tft(&spi, kTftCs, kTftDc, kTftRst);
String serialBuffer;

struct CodexData {
  uint8_t windowPct = 41;
  uint8_t weekPct = 17;
  uint16_t resetMin = 133;
  String resetText = "--:--";
  String syncText = "waiting";
} codex;

unsigned long lastFetchAt = 0;
unsigned long lastWifiAttemptAt = 0;
int wifiIndex = 0;
bool wifiWasConnected = false;

void clearRect(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, ST77XX_BLACK);
}

void drawCenteredText(const String& text, int cx, int y, uint8_t size, uint16_t color, uint16_t bg) {
  tft.setTextColor(color, bg);
  tft.setTextSize(size);
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(cx - static_cast<int>(w) / 2, y);
  tft.print(text);
}

void drawStaticCard(int x, int y, int w, int h) {
  const uint16_t frame = ST77XX_WHITE;
  const uint16_t accent = ST77XX_CYAN;
  const uint16_t muted = ST77XX_BLUE;

  tft.setTextColor(accent, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 14, y + 12);
  tft.print("Codex");

  tft.setTextColor(muted, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(x + 16, y + 32);
  tft.print("remaining");

  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 62);
  tft.print("5h");

  tft.drawRoundRect(x + 16, y + 82, w - 32, 32, 10, muted);

  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 134);
  tft.print("7d");

  tft.drawRoundRect(x + 16, y + 154, w - 32, 22, 9, muted);

  tft.setTextColor(muted, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 192);
  tft.print("Reset");
}

void drawDynamicCard(int x, int y, int w, int h) {
  const uint16_t frame = ST77XX_WHITE;
  const uint16_t accent = ST77XX_CYAN;
  const uint16_t weekAccent = ST77XX_MAGENTA;

  clearRect(x + w - 116, y + 8, 104, 44);
  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(5);
  tft.setCursor(x + w - 110, y + 10);
  tft.print(String(codex.windowPct) + "%");

  clearRect(x + 19, y + 85, w - 38, 26);
  tft.fillRoundRect(x + 19, y + 85, (w - 38) * codex.windowPct / 100, 26, 8, accent);

  clearRect(x + 19, y + 157, w - 38, 16);
  tft.fillRoundRect(x + 19, y + 157, (w - 38) * codex.weekPct / 100, 16, 7, weekAccent);
  clearRect(x + w - 76, y + 131, 62, 18);
  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + w - 72, y + 134);
  tft.print(String(codex.weekPct) + "%");

  clearRect(x + 92, y + 192, 54, 16);
  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 92, y + 192);
  tft.print(codex.resetText);

  clearRect(x + 152, y + 192, 52, 16);
  tft.setTextColor(ST77XX_BLUE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(x + 154, y + 196);
  tft.print(codex.syncText);
}

void renderScreen(bool full) {
  if (full) {
    tft.fillScreen(ST77XX_BLACK);
    drawStaticCard(16, 2, 208, 220);
  }
  drawDynamicCard(16, 2, 208, 220);
}

bool applyJsonPayload(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }

  uint8_t nextWindowPct = doc["window_pct"] | codex.windowPct;
  uint8_t nextWeekPct = doc["week_pct"] | codex.weekPct;
  uint16_t nextResetMin = doc["reset_min"] | codex.resetMin;
  String nextResetText = String(static_cast<const char*>(doc["reset_text"] | codex.resetText.c_str()));

  bool changed = nextWindowPct != codex.windowPct || nextWeekPct != codex.weekPct ||
                 nextResetMin != codex.resetMin || nextResetText != codex.resetText;

  codex.windowPct = nextWindowPct;
  codex.weekPct = nextWeekPct;
  codex.resetMin = nextResetMin;
  codex.resetText = nextResetText;

  if (changed) {
    renderScreen(false);
  }
  return true;
}

void setSyncText(const String& text) {
  if (codex.syncText == text) {
    return;
  }
  codex.syncText = text;
  renderScreen(false);
}

void handleSerialInput() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\n') {
      if (applyJsonPayload(serialBuffer)) {
        setSyncText("serial");
        Serial.println("OK");
      } else {
        setSyncText("ser err");
        Serial.println("ERR");
      }
      serialBuffer = "";
    } else if (ch != '\r') {
      serialBuffer += ch;
    }
  }
}

void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  if (kWifiNetworkCount <= 0) {
    setSyncText("n/a");
    return;
  }
  if (millis() - lastWifiAttemptAt < kWifiRetryMs) {
    return;
  }

  lastWifiAttemptAt = millis();
  setSyncText("wifi...");
  WiFi.disconnect(true, true);
  WiFi.begin(kWifiNetworks[wifiIndex].ssid, kWifiNetworks[wifiIndex].password);
  wifiIndex = (wifiIndex + 1) % kWifiNetworkCount;
}

void fetchFromBridge() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (lastFetchAt != 0 && millis() - lastFetchAt < kFetchIntervalMs) {
    return;
  }

  lastFetchAt = millis();
  HTTPClient http;
  http.begin(kBridgeUrl);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
    if (applyJsonPayload(body)) {
      setSyncText("ok");
    } else {
      setSyncText("json");
    }
  } else {
    setSyncText("http");
  }
  http.end();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(kBacklight, OUTPUT);
  digitalWrite(kBacklight, HIGH);

  spi.begin(kTftSclk, -1, kTftMosi, kTftCs);
  tft.init(240, 240, SPI_MODE0);
  tft.setRotation(0);
  tft.invertDisplay(true);

  WiFi.mode(WIFI_STA);
  renderScreen(true);
  Serial.println("codex-card: ready");
}

void loop() {
  handleSerialInput();
  ensureWifiConnected();

  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !wifiWasConnected) {
    lastFetchAt = 0;
    setSyncText("wifi");
  }
  wifiWasConnected = wifiConnected;

  fetchFromBridge();
  delay(50);
}
