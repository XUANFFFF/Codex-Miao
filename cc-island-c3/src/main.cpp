#include <Arduino.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

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
  float costToday = -1.0f;
  uint32_t tokensToday = 0;
  uint16_t resetMin = 133;
  String resetText = "--:--";
} codex;

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

  tft.drawRoundRect(x, y, w, h, 14, frame);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 14, muted);

  tft.setTextColor(accent, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 16);
  tft.print("Codex");

  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 46);
  tft.print("5h");

  tft.setTextSize(5);
  tft.setCursor(x + 16, y + 70);
  tft.print(String(codex.windowPct) + "%");

  tft.drawRoundRect(x + 16, y + 116, w - 32, 24, 9, muted);
  tft.fillRoundRect(x + 19, y + 119, (w - 38) * codex.windowPct / 100, 18, 7, accent);

  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 150);
  tft.print("7d");

  tft.drawRoundRect(x + 16, y + 176, w - 32, 24, 9, muted);
  tft.fillRoundRect(x + 19, y + 179, (w - 38) * codex.weekPct / 100, 18, 7, weekAccent);
  drawCenteredText(String(codex.weekPct) + "%", x + w / 2, y + 181, 2, ST77XX_BLACK, weekAccent);

  tft.setTextColor(muted, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x + 16, y + 208);
  tft.print("Reset");
  tft.setTextColor(frame, ST77XX_BLACK);
  tft.setCursor(x + 92, y + 208);
  tft.print(codex.resetText);
}

void renderScreen() {
  tft.fillScreen(ST77XX_BLACK);
  drawCard(16, 10, 208, 220);
}

bool updateFromJson(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }

  codex.windowPct = doc["window_pct"] | codex.windowPct;
  codex.weekPct = doc["week_pct"] | codex.weekPct;
  codex.costToday = doc["cost_today"] | codex.costToday;
  codex.tokensToday = doc["tokens_today"] | codex.tokensToday;
  codex.resetMin = doc["reset_min"] | codex.resetMin;
  codex.resetText = String(static_cast<const char*>(doc["reset_text"] | codex.resetText.c_str()));
  renderScreen();
  return true;
}

void handleSerialInput() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\n') {
      if (updateFromJson(serialBuffer)) {
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

  renderScreen();
  Serial.println("codex-card: ready");
}

void loop() {
  handleSerialInput();
  delay(10);
}
