#include <Arduino.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <string.h>

#include "display_types.h"
#include "face_renderer.h"
#include "screen_mode_controller.h"
#include "usage_card_renderer.h"
#include "usage_state_tracker.h"
#include "wifi_config.h"

namespace {
constexpr int kTftSclk = 4;
constexpr int kTftMosi = 6;
constexpr int kTftCs = 7;
constexpr int kTftDc = 2;
constexpr int kTftRst = 3;
constexpr int kBacklight = 10;
constexpr int16_t kScreenWidth = 240;
constexpr int16_t kScreenHeight = 240;
constexpr size_t kSerialBufferLimit = 384;
constexpr size_t kJsonDocCapacity = 384;
constexpr uint8_t kTransitionStepsPerHalf = 8;
constexpr uint8_t kTransitionPhaseCount = kTransitionStepsPerHalf * 2;

SPIClass spi(FSPI);
Adafruit_ST7789 tft(&spi, kTftCs, kTftDc, kTftRst);
FaceRenderer faceRenderer(tft);
ScreenModeController screenModeController;
UsageCardRenderer usageCardRenderer(tft);
UsageStateTracker usageStateTracker;
UsageData usageData;
String serialBuffer;
bool serialLineOverflowed = false;
ScreenMode lastRenderedMode = ScreenMode::Face;
bool usageCardDirty = true;
bool transitionWasActive = false;
TransitionDirection lastTransitionDirection = TransitionDirection::None;
ScreenMode lastTransitionTargetMode = ScreenMode::Face;
uint8_t lastTransitionPhase = 0xFF;

unsigned long lastFetchAt = 0;
unsigned long lastWifiAttemptAt = 0;
int wifiIndex = 0;
bool wifiWasConnected = false;

void copyBoundedText(char* dest, size_t capacity, const char* src) {
  if (capacity == 0 || dest == nullptr) {
    return;
  }

  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }

  strncpy(dest, src, capacity);
  dest[capacity - 1] = '\0';
}

bool boundedTextEquals(const char* left, const char* right, size_t capacity) {
  if (left == right) {
    return true;
  }
  if (left == nullptr || right == nullptr) {
    return false;
  }
  return strncmp(left, right, capacity) == 0;
}

uint8_t clampPercent(long value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return static_cast<uint8_t>(value);
}

uint16_t clampResetMinutes(long value) {
  if (value < 0) {
    return 0;
  }
  if (value > 65535L) {
    return 65535;
  }
  return static_cast<uint16_t>(value);
}

void renderMode(ScreenMode mode, bool fullRedraw) {
  if (mode == ScreenMode::UsageCard) {
    usageCardRenderer.renderFrame(usageData, fullRedraw);
  } else {
    faceRenderer.render(fullRedraw);
  }
}

int16_t computeCoverWidth(uint8_t progress) {
  if (progress >= kTransitionPhaseCount) {
    return 0;
  }

  if (progress <= kTransitionStepsPerHalf) {
    return static_cast<int16_t>((static_cast<uint32_t>(progress) * kScreenWidth) /
                                kTransitionStepsPerHalf);
  }

  return static_cast<int16_t>((static_cast<uint32_t>(kTransitionPhaseCount - progress) *
                               kScreenWidth) /
                              kTransitionStepsPerHalf);
}

uint8_t computeTransitionPhase(uint8_t progress) {
  const uint16_t scaled = static_cast<uint16_t>(progress) * kTransitionPhaseCount;
  uint8_t phase = static_cast<uint8_t>(scaled / 255U);
  if (phase > kTransitionPhaseCount) {
    phase = kTransitionPhaseCount;
  }
  return phase;
}

void drawTransitionCover(TransitionDirection direction, uint8_t phase) {
  const int16_t coverWidth = computeCoverWidth(phase);
  if (coverWidth <= 0) {
    return;
  }

  const bool coverFromLeft = direction == TransitionDirection::FaceToCard;
  const int16_t coverX = coverFromLeft ? 0 : (kScreenWidth - coverWidth);
  tft.fillRect(coverX, 0, coverWidth, kScreenHeight, ST77XX_BLACK);
}

void resetFaceToIdle() {
  faceRenderer.clearExpressionHold();
  faceRenderer.setAutoCycleEnabled(true);
  faceRenderer.setExpression(FaceExpression::Neutral);
}

void updateDisplay() {
  const uint32_t nowMs = millis();
  const ScreenMode desiredMode =
      (usageStateTracker.hasData() && usageStateTracker.isUsageActive(nowMs))
          ? ScreenMode::UsageCard
          : ScreenMode::Face;

  screenModeController.setMode(desiredMode, nowMs);
  screenModeController.update(nowMs);

  const ScreenMode currentMode = screenModeController.currentMode();
  if (currentMode == ScreenMode::Face && lastRenderedMode != ScreenMode::Face) {
    resetFaceToIdle();
  }

  const TransitionState& transition = screenModeController.transition();
  const bool fullRedraw = screenModeController.needsFullRedraw();
  const ScreenMode targetMode = screenModeController.targetMode();
  const bool transitionJustStarted =
      transition.active &&
      (!transitionWasActive || transition.direction != lastTransitionDirection ||
       targetMode != lastTransitionTargetMode);

  if (transitionJustStarted && targetMode == ScreenMode::Face) {
    resetFaceToIdle();
  }

  const bool faceChanged = faceRenderer.update(nowMs);

  if (transition.active) {
    const uint8_t transitionPhase = computeTransitionPhase(transition.progress);
    if (transitionPhase != lastTransitionPhase || fullRedraw || transitionJustStarted) {
      const bool revealTarget = transitionPhase > kTransitionStepsPerHalf;
      if (revealTarget) {
        const bool revealNeedsFullRedraw =
            lastTransitionPhase <= kTransitionStepsPerHalf || targetMode == ScreenMode::UsageCard;
        renderMode(targetMode, revealNeedsFullRedraw);
        if (targetMode == ScreenMode::UsageCard) {
          usageCardDirty = false;
        }
      } else if (transitionJustStarted || fullRedraw) {
        renderMode(currentMode, true);
        if (currentMode == ScreenMode::UsageCard) {
          usageCardDirty = false;
        }
      }

      drawTransitionCover(transition.direction, transitionPhase);
      lastTransitionPhase = transitionPhase;
    }
    transitionWasActive = true;
    lastTransitionDirection = transition.direction;
    lastTransitionTargetMode = targetMode;
  } else {
    const bool modeChanged = currentMode != lastRenderedMode;
    if (currentMode == ScreenMode::UsageCard) {
      if (fullRedraw || modeChanged || usageCardDirty) {
        renderMode(currentMode, fullRedraw || modeChanged);
        usageCardDirty = false;
      }
    } else if (fullRedraw || modeChanged || faceChanged) {
      renderMode(currentMode, fullRedraw || modeChanged);
    }

    lastRenderedMode = currentMode;
    transitionWasActive = false;
    lastTransitionDirection = TransitionDirection::None;
    lastTransitionTargetMode = currentMode;
    lastTransitionPhase = 0xFF;
  }

  if (fullRedraw) {
    screenModeController.consumeFullRedrawFlag();
  }
}

bool applyJsonPayload(const String& line) {
  StaticJsonDocument<kJsonDocCapacity> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }

  const char* displayMode = doc["display_mode"] | "";
  const char* bridgeState = doc["bridge_state"] | "";
  if (strcmp(displayMode, "face") == 0 || strcmp(bridgeState, "offline") == 0) {
    usageStateTracker.forceIdle();
    usageCardDirty = true;
    updateDisplay();
    return true;
  }

  UsageData nextData = usageData;

  if (doc["window_pct"].is<long>()) {
    nextData.windowPct = clampPercent(doc["window_pct"].as<long>());
  }

  if (doc["week_pct"].is<long>()) {
    nextData.weekPct = clampPercent(doc["week_pct"].as<long>());
  }

  if (doc["reset_min"].is<long>()) {
    nextData.resetMin = clampResetMinutes(doc["reset_min"].as<long>());
  }

  if (doc["reset_text"].is<const char*>()) {
    copyBoundedText(nextData.resetText, kResetTextCapacity, doc["reset_text"].as<const char*>());
  }

  const bool dataChanged =
      nextData.windowPct != usageData.windowPct || nextData.weekPct != usageData.weekPct ||
      nextData.resetMin != usageData.resetMin ||
      !boundedTextEquals(nextData.resetText, usageData.resetText, kResetTextCapacity);

  usageData = nextData;
  const bool hasMeaningfulActivity = usageStateTracker.applyIncomingData(usageData, millis());
  if (hasMeaningfulActivity) {
    faceRenderer.setExpression(FaceExpression::Happy);
    faceRenderer.setAutoCycleEnabled(true);
  }

  if (dataChanged || hasMeaningfulActivity) {
    usageCardDirty = true;
    updateDisplay();
  }

  return true;
}

void setSyncText(const char* text) {
  if (boundedTextEquals(usageData.syncText, text, kSyncTextCapacity)) {
    return;
  }

  copyBoundedText(usageData.syncText, kSyncTextCapacity, text);
  usageCardDirty = true;
  updateDisplay();
}

void handleSerialInput() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\n') {
      if (!serialLineOverflowed && applyJsonPayload(serialBuffer)) {
        setSyncText("serial");
        Serial.println("OK");
      } else {
        setSyncText("ser err");
        Serial.println("ERR");
      }
      serialBuffer = "";
      serialLineOverflowed = false;
    } else if (ch != '\r') {
      if (serialLineOverflowed) {
        continue;
      }

      if (serialBuffer.length() >= kSerialBufferLimit) {
        serialBuffer = "";
        serialLineOverflowed = true;
        continue;
      }

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

  faceRenderer.begin();
  WiFi.mode(WIFI_STA);
  updateDisplay();
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
  updateDisplay();
  delay(25);
}
