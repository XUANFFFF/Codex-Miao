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

void drawCard(int x, int y, int w, int h) {
  const uint16_t frame = ST77XX_WHITE;
  const uint16_t accent = ST77XX_CYAN;
  const uint16_t muted = ST77XX_BLUE;
  const uint16_t weekAccent = ST77XX_MAGENTA;

  tft.setTextColor(accent, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 16);
  tft.print("Codex");

  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 46);
  tft.print("5h");

  tft.setTextSize(4);
  tft.setCursor(x + w - 86, y + 42);
  tft.print(String(codex.windowPct) + "%");

  tft.drawRoundRect(x + 16, y + 84, w - 32, 34, 11, muted);
  tft.fillRoundRect(x + 19, y + 87, (w - 38) * codex.windowPct / 100, 28, 8, accent);

  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 132);
  tft.print("7d");

  tft.drawRoundRect(x + 16, y + 158, w - 32, 24, 9, muted);
  tft.fillRoundRect(x + 19, y + 161, (w - 38) * codex.weekPct / 100, 18, 7, weekAccent);
  drawCenteredText(String(codex.weekPct) + "%", x + w / 2, y + 163, 2, ST77XX_WHITE, ST77XX_BLACK);

  tft.setTextColor(muted, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 198);
  tft.print("Reset");
  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setCursor(x + 92, y + 198);
  tft.print(codex.resetText);

  tft.setTextColor(muted, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(x + 16, y + 224);
  tft.print(codex.syncText);
}

void renderScreen() {
  tft.fillScreen(ST77XX_BLACK);
  drawCard(16, 10, 208, 220);
}

bool applyJsonPayload(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }

  codex.windowPct = doc["window_pct"] | codex.windowPct;
  codex.weekPct = doc["week_pct"] | codex.weekPct;
  codex.resetMin = doc["reset_min"] | codex.resetMin;
  codex.resetText = String(static_cast<const char*>(doc["reset_text"] | codex.resetText.c_str()));
  renderScreen();
  return true;
}

void handleSerialInput() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\n') {
      if (applyJsonPayload(serialBuffer)) {
        codex.syncText = "serial sync ok";
        renderScreen();
        Serial.println("OK");
      } else {
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
    codex.syncText = "wifi not set";
    renderScreen();
    return;
  }
  if (millis() - lastWifiAttemptAt < kWifiRetryMs) {
    return;
  }

  lastWifiAttemptAt = millis();
  codex.syncText = "wifi " + String(wifiIndex + 1) + " connecting";
  renderScreen();
  WiFi.disconnect(true, true);
  WiFi.begin(kWifiNetworks[wifiIndex].ssid, kWifiNetworks[wifiIndex].password);
  wifiIndex = (wifiIndex + 1) % kWifiNetworkCount;
}

void fetchFromBridge() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (millis() - lastFetchAt < kFetchIntervalMs) {
    return;
  }

  lastFetchAt = millis();
  HTTPClient http;
  http.begin(kBridgeUrl);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
    if (applyJsonPayload(body)) {
      codex.syncText = "wifi sync ok";
    } else {
      codex.syncText = "json parse err";
    }
  } else {
    codex.syncText = "http err " + String(httpCode);
  }
  http.end();
  renderScreen();
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
  renderScreen();
  Serial.println("codex-card: ready");
}

void loop() {
  handleSerialInput();
  ensureWifiConnected();
  fetchFromBridge();

  if (WiFi.status() == WL_CONNECTED && codex.syncText == "wifi connecting") {
    codex.syncText = "wifi connected";
    renderScreen();
  }

  delay(50);
}
