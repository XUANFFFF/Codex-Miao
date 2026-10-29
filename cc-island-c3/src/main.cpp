#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <WiFi.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>

#include "bridge_discovery.h"
#include "display_types.h"
#include "face_renderer.h"
#include "screen_mode_controller.h"
#include "signal_page_renderer.h"
#include "status_order.h"
#include "usage_card_renderer.h"
#include "usage_state_tracker.h"
#include "wifi_config.h"

#define TFT_CS 7
#define TFT_DC 5
#define TFT_RST 3
#define TFT_BL 10

// Comment this out to use init scheme A: tft.init(240, 240);
#define USE_INIT_SCHEME_B

namespace {
constexpr int kTftSclk = 4;
constexpr int kTftMosi = 6;
constexpr int16_t kScreenWidth = 240;
constexpr int16_t kScreenHeight = 240;
constexpr size_t kSerialBufferLimit = 1024;
constexpr size_t kJsonDocCapacity = 384;
constexpr uint8_t kTransitionStepsPerHalf = 8;
constexpr uint8_t kTransitionPhaseCount = kTransitionStepsPerHalf * 2;
constexpr uint32_t kSignalPageHoldMs = 1800;

SPIClass spi(FSPI);
Adafruit_ST7789 tft(&spi, TFT_CS, TFT_DC, TFT_RST);
FaceRenderer faceRenderer(tft);
ScreenModeController screenModeController;
UsageCardRenderer usageCardRenderer(tft);
SignalPageRenderer signalPageRenderer(tft);
UsageStateTracker usageStateTracker;
BridgeDiscovery bridgeDiscovery;
UsageData usageData;
SignalOverlayState signalOverlayState;
SignalPageState signalPageState;
String serialBuffer;
bool serialLineOverflowed = false;
ScreenMode lastRenderedMode = ScreenMode::Face;
bool usageCardDirty = true;
bool transitionWasActive = false;
TransitionDirection lastTransitionDirection = TransitionDirection::None;
ScreenMode lastTransitionTargetMode = ScreenMode::Face;
uint8_t lastTransitionPhase = 0xFF;
char activeBridgeUrl[96] = {0};
bool endpointWasAvailable = false;
bool lastUsageActive = false;
bool lastHasUsageData = false;
ScreenMode lastDesiredModeLogged = ScreenMode::Face;
bool lastSignalPageVisible = false;
SignalPageKind lastRenderedSignalPageKind = SignalPageKind::None;

unsigned long nextFetchAllowedAt = 0;
unsigned long lastWifiAttemptAt = 0;
int wifiIndex = 0;
bool wifiWasConnected = false;
StatusOrder statusOrder;
uint32_t lastStatusAtMs = 0;
uint32_t lastSerialStatusAtMs = 0;
bool hasStatus = false;
uint32_t statusReceiptCount = 0;
bool hasSerialStatus = false;
uint8_t activeSessionCount = 0;
uint8_t permissionSessionCount = 0;
uint8_t completedSessionCount = 0;

bool makeBridgeRequestSignature(const char* requestTarget, const char* nonce, char* signature,
                                size_t capacity) {
  if (requestTarget == nullptr || nonce == nullptr || signature == nullptr || capacity < 65) {
    return false;
  }

  char message[160];
  const int messageLength = snprintf(message, sizeof(message), "GET\n%s\n%s", requestTarget, nonce);
  if (messageLength <= 0 || static_cast<size_t>(messageLength) >= sizeof(message)) {
    return false;
  }

  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mdInfo == nullptr) {
    return false;
  }

  unsigned char digest[32];
  if (mbedtls_md_hmac(mdInfo, reinterpret_cast<const unsigned char*>(kBridgeAuthToken),
                      strlen(kBridgeAuthToken), reinterpret_cast<const unsigned char*>(message),
                      static_cast<size_t>(messageLength), digest) != 0) {
    return false;
  }

  static constexpr char kHexDigits[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(digest); ++index) {
    signature[index * 2] = kHexDigits[digest[index] >> 4];
    signature[index * 2 + 1] = kHexDigits[digest[index] & 0x0F];
  }
  signature[64] = '\0';
  return true;
}

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

bool isRedSignal(AgentSignalState signal) {
  return signal == AgentSignalState::Permission || signal == AgentSignalState::Blocked;
}

SignalPageKind signalPageKindFor(AgentSignalState signal) {
  switch (signal) {
    case AgentSignalState::Permission:
      return SignalPageKind::Permission;
    case AgentSignalState::Blocked:
      return SignalPageKind::Blocked;
    case AgentSignalState::Idle:
    case AgentSignalState::Thinking:
    case AgentSignalState::Working:
    default:
      return SignalPageKind::None;
  }
}

bool parseAgentSignal(const JsonDocument& doc, AgentSignalState& signal) {
  if (!doc["agent_signal"].is<const char*>()) {
    return false;
  }

  const char* value = doc["agent_signal"].as<const char*>();
  if (value == nullptr || value[0] == '\0') {
    return false;
  }

  if (strcmp(value, "idle") == 0) {
    signal = AgentSignalState::Idle;
    return true;
  }
  if (strcmp(value, "thinking") == 0) {
    signal = AgentSignalState::Thinking;
    return true;
  }
  if (strcmp(value, "working") == 0) {
    signal = AgentSignalState::Working;
    return true;
  }
  if (strcmp(value, "permission") == 0) {
    signal = AgentSignalState::Permission;
    return true;
  }
  if (strcmp(value, "blocked") == 0) {
    signal = AgentSignalState::Blocked;
    return true;
  }
  if (strcmp(value, "done") == 0) {
    signal = AgentSignalState::Done;
    return true;
  }
  if (strcmp(value, "unknown") == 0) {
    signal = AgentSignalState::Unknown;
    return true;
  }

  return false;
}

void clearSignalPage() {
  signalPageState.kind = SignalPageKind::None;
  signalPageState.holdUntilMs = 0;
}

void updateSignalState(AgentSignalState nextSignal, uint32_t nowMs) {
  const bool wasRed = isRedSignal(signalOverlayState.signal);
  const bool isRed = isRedSignal(nextSignal);

  signalOverlayState.signal = nextSignal;
  signalOverlayState.visible = true;
  if (nextSignal == AgentSignalState::Done) faceRenderer.setExpression(FaceExpression::Happy);

  if (isRed) {
    signalPageState.kind = signalPageKindFor(nextSignal);
    signalPageState.holdUntilMs = 0;
    return;
  }

  if (wasRed && signalPageState.kind != SignalPageKind::None) {
    signalPageState.holdUntilMs = nowMs + kSignalPageHoldMs;
  }
}

bool shouldShowSignalPage(uint32_t nowMs) {
  if (signalPageState.kind == SignalPageKind::None) {
    return false;
  }

  if (isRedSignal(signalOverlayState.signal)) {
    return true;
  }

  if (signalPageState.holdUntilMs != 0 &&
      static_cast<int32_t>(signalPageState.holdUntilMs - nowMs) > 0) {
    return true;
  }

  clearSignalPage();
  return false;
}

ScreenMode desiredContentMode(uint32_t nowMs) {
  if (isRedSignal(signalOverlayState.signal)) {
    return ScreenMode::Face;
  }

  if (signalOverlayState.signal == AgentSignalState::Thinking ||
      signalOverlayState.signal == AgentSignalState::Working ||
      usageStateTracker.isUsageActive(nowMs)) {
    return ScreenMode::UsageCard;
  }

  return ScreenMode::Face;
}

SignalOverlayState overlayForMode(ScreenMode mode) {
  (void)mode;
  return signalOverlayState;
}

void renderMode(ScreenMode mode, bool fullRedraw, const SignalOverlayState& overlay) {
  if (mode == ScreenMode::UsageCard) {
    usageCardRenderer.renderFrame(usageData, fullRedraw, overlay);
  } else {
    faceRenderer.render(fullRedraw, overlay);
  }
  tft.fillRect(4, 220, 170, 12, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(4, 222);
  char summary[40];
  if (overlay.signal == AgentSignalState::Unknown) {
    snprintf(summary, sizeof(summary), "STATUS UNKNOWN");
  } else {
    snprintf(summary, sizeof(summary), "W:%u OK?:%u DONE:%u", activeSessionCount,
             permissionSessionCount, completedSessionCount);
  }
  tft.print(summary);
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
  faceRenderer.setExpression(signalOverlayState.signal == AgentSignalState::Done
                                ? FaceExpression::Happy : FaceExpression::Neutral);
}

void updateDisplay() {
  const uint32_t nowMs = millis();
  const bool hasUsageData = usageStateTracker.hasData();
  const bool usageActive = usageStateTracker.isUsageActive(nowMs);
  const ScreenMode desiredMode = desiredContentMode(nowMs);
  const bool showSignalPage = shouldShowSignalPage(nowMs);

  if (usageActive != lastUsageActive || hasUsageData != lastHasUsageData ||
      desiredMode != lastDesiredModeLogged) {
    Serial.printf("display: hasData=%s active=%s activeUntil=%lu now=%lu mode=%s\n",
                  hasUsageData ? "true" : "false", usageActive ? "true" : "false",
                  static_cast<unsigned long>(usageStateTracker.activeUntilMs()),
                  static_cast<unsigned long>(nowMs),
                  desiredMode == ScreenMode::UsageCard ? "usage" : "face");
    lastUsageActive = usageActive;
    lastHasUsageData = hasUsageData;
    lastDesiredModeLogged = desiredMode;
  }

  screenModeController.setMode(desiredMode, nowMs);
  screenModeController.update(nowMs);

  const ScreenMode currentMode = screenModeController.currentMode();
  if (currentMode == ScreenMode::Face && lastRenderedMode != ScreenMode::Face) {
    resetFaceToIdle();
  }

  const TransitionState& transition = screenModeController.transition();
  const bool controllerFullRedraw = screenModeController.needsFullRedraw();
  const bool fullRedraw = controllerFullRedraw || (!showSignalPage && lastSignalPageVisible);
  const ScreenMode targetMode = screenModeController.targetMode();
  const bool transitionJustStarted =
      transition.active &&
      (!transitionWasActive || transition.direction != lastTransitionDirection ||
       targetMode != lastTransitionTargetMode);

  if (transitionJustStarted && targetMode == ScreenMode::Face) {
    resetFaceToIdle();
  }

  const bool faceChanged = faceRenderer.update(nowMs);

  if (showSignalPage) {
    const bool pageChanged =
        signalPageState.kind != lastRenderedSignalPageKind || !lastSignalPageVisible;
    if (fullRedraw || pageChanged) {
      signalPageRenderer.render(signalPageState, true);
      lastRenderedSignalPageKind = signalPageState.kind;
    }

    lastSignalPageVisible = true;
    transitionWasActive = false;
    lastTransitionDirection = TransitionDirection::None;
    lastTransitionTargetMode = currentMode;
    lastTransitionPhase = 0xFF;
    lastRenderedMode = currentMode;

    if (controllerFullRedraw) {
      screenModeController.consumeFullRedrawFlag();
    }
    return;
  }

  if (transition.active) {
    const uint8_t transitionPhase = computeTransitionPhase(transition.progress);
    if (transitionPhase != lastTransitionPhase || fullRedraw || transitionJustStarted) {
      const bool revealTarget = transitionPhase > kTransitionStepsPerHalf;
      if (revealTarget) {
        const bool revealNeedsFullRedraw =
            lastTransitionPhase <= kTransitionStepsPerHalf || targetMode == ScreenMode::UsageCard;
        renderMode(targetMode, revealNeedsFullRedraw, overlayForMode(targetMode));
        if (targetMode == ScreenMode::UsageCard) {
          usageCardDirty = false;
        }
      } else if (transitionJustStarted || fullRedraw) {
        renderMode(currentMode, true, overlayForMode(currentMode));
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
        renderMode(currentMode, fullRedraw || modeChanged, overlayForMode(currentMode));
        usageCardDirty = false;
      }
    } else if (fullRedraw || modeChanged || faceChanged || usageCardDirty) {
      renderMode(currentMode, fullRedraw || modeChanged, overlayForMode(currentMode));
      usageCardDirty = false;
    }

    lastRenderedMode = currentMode;
    transitionWasActive = false;
    lastTransitionDirection = TransitionDirection::None;
    lastTransitionTargetMode = currentMode;
    lastTransitionPhase = 0xFF;
  }

  lastSignalPageVisible = false;
  lastRenderedSignalPageKind = SignalPageKind::None;

  if (controllerFullRedraw) {
    screenModeController.consumeFullRedrawFlag();
  }
}

bool applyJsonPayload(const String& line) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }
  if (!doc.is<JsonObject>()) return false;

  if (doc["command"] == "health") {
    Serial.printf("{\"device\":\"codex-miao-c3\",\"protocol_version\":2,\"has_status\":%s,\"status_age_ms\":%lu,\"free_heap\":%lu}\n",
                  hasStatus ? "true" : "false",
                  static_cast<unsigned long>(millis() - lastStatusAtMs),
                  static_cast<unsigned long>(ESP.getFreeHeap()));
    return true;
  }
  const bool ordered = doc["status_stream"].is<const char*>() && doc["status_revision"].is<uint64_t>();
  AgentSignalState validSignal;
  const bool validStatus = parseAgentSignal(doc, validSignal);
  if ((ordered && !validStatus) || (!validStatus && !doc["window_pct"].is<long>() &&
                                   !doc["week_pct"].is<long>())) return false;
  if (ordered) {
    if (!statusOrder.accept(doc["status_stream"].as<const char*>(), doc["status_revision"].as<uint64_t>())) {
      return true;  // Acknowledge duplicate transport work without applying old data.
    }
  } else if (statusOrder.initialized()) {
    return true;
  }

  const char* displayMode = doc["display_mode"] | "";
  const char* bridgeState = doc["bridge_state"] | "";
  const uint32_t nowMs = millis();
  hasStatus = true;
  ++statusReceiptCount;
  lastStatusAtMs = nowMs;
  const uint8_t nextActive = clampPercent(doc["active_sessions"] | 0L);
  const uint8_t nextPermission = clampPercent(doc["permission_sessions"] | 0L);
  const uint8_t nextCompleted = clampPercent(doc["completed_sessions"] | 0L);
  if (nextActive != activeSessionCount || nextPermission != permissionSessionCount ||
      nextCompleted != completedSessionCount) usageCardDirty = true;
  activeSessionCount = nextActive;
  permissionSessionCount = nextPermission;
  completedSessionCount = nextCompleted;

  const AgentSignalState previousSignal = signalOverlayState.signal;
  AgentSignalState parsedSignal = signalOverlayState.signal;
  const bool hasParsedSignal = parseAgentSignal(doc, parsedSignal);
  if (hasParsedSignal) {
    updateSignalState(parsedSignal, nowMs);
  }
  const bool signalChanged = signalOverlayState.signal != previousSignal;

  if (strcmp(displayMode, "face") == 0 || strcmp(bridgeState, "offline") == 0) {
    updateSignalState(AgentSignalState::Idle, nowMs);
    clearSignalPage();
    usageStateTracker.forceIdle();
    usageCardDirty = true;
    updateDisplay();
    return true;
  }

  UsageData nextData = usageData;
  const bool hasUsagePercentages = doc["window_pct"].is<long>() || doc["week_pct"].is<long>();
  if (doc["usage_available"].is<bool>()) {
    nextData.usageAvailable = doc["usage_available"].as<bool>();
  } else if (hasUsagePercentages) {
    nextData.usageAvailable = true;
  }

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
      nextData.usageAvailable != usageData.usageAvailable ||
      nextData.windowPct != usageData.windowPct || nextData.weekPct != usageData.weekPct ||
      nextData.resetMin != usageData.resetMin ||
      !boundedTextEquals(nextData.resetText, usageData.resetText, kResetTextCapacity);

  usageData = nextData;
  const bool hasMeaningfulActivity = usageStateTracker.applyIncomingData(usageData, nowMs);
  if (hasMeaningfulActivity) {
    faceRenderer.setExpression(FaceExpression::Happy);
    faceRenderer.setAutoCycleEnabled(true);
  }

  if (dataChanged || hasMeaningfulActivity || signalChanged) {
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
      const uint32_t previousReceipt = statusReceiptCount;
      if (!serialLineOverflowed && applyJsonPayload(serialBuffer)) {
        if (statusReceiptCount != previousReceipt) {
          hasSerialStatus = true;
          lastSerialStatusAtMs = millis();
        }
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
  Serial.printf("wifi: connecting ssid=%s\n", kWifiNetworks[wifiIndex].ssid);
  setSyncText("wifi...");
  WiFi.disconnect(true, true);
  WiFi.begin(kWifiNetworks[wifiIndex].ssid, kWifiNetworks[wifiIndex].password);
  wifiIndex = (wifiIndex + 1) % kWifiNetworkCount;
}

void fetchFromBridge(uint32_t nowMs) {
  if (hasSerialStatus && nowMs - lastSerialStatusAtMs < 10000) return;
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (nextFetchAllowedAt != 0 && static_cast<int32_t>(nowMs - nextFetchAllowedAt) < 0) {
    return;
  }
  if (!bridgeDiscovery.endpointUrl(activeBridgeUrl, sizeof(activeBridgeUrl), kBridgeUsagePath)) {
    return;
  }

  const bool hadEndpoint = bridgeDiscovery.hasEndpoint();
  Serial.printf("fetch: GET %s\n", activeBridgeUrl);

  char challengeUrl[96];
  if (!bridgeDiscovery.endpointUrl(challengeUrl, sizeof(challengeUrl), kBridgeChallengePath)) {
    return;
  }

  HTTPClient challengeHttp;
  if (!challengeHttp.begin(challengeUrl)) {
    bridgeDiscovery.noteFetchFailure(nowMs);
    nextFetchAllowedAt = nowMs + kFetchRetryAfterFailureMs;
    setSyncText("http");
    return;
  }
  const int challengeCode = challengeHttp.GET();
  String challengeBody;
  if (challengeCode == HTTP_CODE_OK) {
    challengeBody = challengeHttp.getString();
  }
  challengeHttp.end();

  JsonDocument challengeDoc;
  const char* nonce = "";
  if (challengeCode == HTTP_CODE_OK && deserializeJson(challengeDoc, challengeBody) == DeserializationError::Ok) {
    nonce = challengeDoc["nonce"] | "";
  }

  char signature[65];
  if (strlen(nonce) != 32 ||
      !makeBridgeRequestSignature(kBridgeUsagePath, nonce, signature, sizeof(signature))) {
    bridgeDiscovery.noteFetchFailure(nowMs);
    nextFetchAllowedAt = nowMs + kFetchRetryAfterFailureMs;
    Serial.printf("fetch: challenge failed code=%d\n", challengeCode);
    setSyncText("");
    if (hadEndpoint && !bridgeDiscovery.hasEndpoint()) {
      setSyncText("http");
    }
    return;
  }

  HTTPClient http;
  http.begin(activeBridgeUrl);
  http.addHeader("X-Codex-Miao-Nonce", nonce);
  http.addHeader("X-Codex-Miao-Signature", signature);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
    if (applyJsonPayload(body)) {
      bridgeDiscovery.noteFetchSuccess(nowMs);
      nextFetchAllowedAt = nowMs + kFetchIntervalMs;
      Serial.println("fetch: ok");
      setSyncText("ok");
    } else {
      bridgeDiscovery.noteFetchFailure(nowMs);
      nextFetchAllowedAt = nowMs + kFetchRetryAfterFailureMs;
      Serial.println("fetch: json parse failed");
      setSyncText("");
      if (hadEndpoint && !bridgeDiscovery.hasEndpoint()) {
        setSyncText("json");
      }
    }
  } else {
    bridgeDiscovery.noteFetchFailure(nowMs);
    nextFetchAllowedAt = nowMs + kFetchRetryAfterFailureMs;
    Serial.printf("fetch: http failed code=%d\n", httpCode);
    setSyncText("");
    if (hadEndpoint && !bridgeDiscovery.hasEndpoint()) {
      setSyncText("http");
    }
  }
  http.end();
}
}  // namespace

void setup() {
  // HWCDC defaults to 256 bytes; a full status line can arrive while drawing
  // or fetching HTTP. Keep room for the maximum line plus queued diagnostics.
  Serial.setRxBufferSize(2 * (kSerialBufferLimit + 1));
  Serial.begin(115200);
  delay(800);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(kTftSclk, -1, kTftMosi, TFT_CS);
  spi.begin(kTftSclk, -1, kTftMosi, TFT_CS);
#ifdef USE_INIT_SCHEME_B
  tft.init(240, 240, SPI_MODE3);
#else
  tft.init(240, 240);
#endif
  tft.setRotation(2);

  faceRenderer.begin();
  updateSignalState(AgentSignalState::Unknown, millis());
  WiFi.mode(WIFI_STA);
  bridgeDiscovery.begin(kBridgeDiscoveryPort, kBridgeDiscoveryService, kBridgeDiscoveryStaleMs);
  updateDisplay();
  Serial.println("codex-card: ready");
}

void loop() {
  handleSerialInput();
  const bool serialFresh = hasSerialStatus && millis() - lastSerialStatusAtMs < 10000;
  ensureWifiConnected();

  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !wifiWasConnected) {
    nextFetchAllowedAt = 0;
    bridgeDiscovery.reset();
    endpointWasAvailable = false;
    Serial.printf("wifi: connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    setSyncText("wifi");
  } else if (!wifiConnected && wifiWasConnected) {
    nextFetchAllowedAt = 0;
    bridgeDiscovery.reset();
    endpointWasAvailable = false;
    if (!serialFresh) {
      updateSignalState(AgentSignalState::Unknown, millis());
      clearSignalPage();
      usageStateTracker.forceIdle();
    }
    Serial.println("wifi: disconnected");
    setSyncText("");
  }
  wifiWasConnected = wifiConnected;

  if (wifiConnected) {
    const uint32_t nowMs = millis();
    bridgeDiscovery.update(nowMs);
    const bool hasEndpoint = bridgeDiscovery.hasEndpoint();
    if (hasEndpoint != endpointWasAvailable) {
      endpointWasAvailable = hasEndpoint;
      if (hasEndpoint) {
        if (bridgeDiscovery.endpointUrl(activeBridgeUrl, sizeof(activeBridgeUrl),
                                       kBridgeUsagePath)) {
          Serial.printf("discovery: active endpoint %s\n", activeBridgeUrl);
        } else {
          Serial.println("discovery: active endpoint ready");
        }
      } else {
        Serial.println("discovery: waiting for beacon");
      }
    }
    if (!hasEndpoint && !serialFresh) {
      if (nextFetchAllowedAt != 0) {
        nextFetchAllowedAt = 0;
        setSyncText("");
      }
      updateSignalState(AgentSignalState::Unknown, nowMs);
      clearSignalPage();
      usageStateTracker.forceIdle();
      updateDisplay();
    }

    fetchFromBridge(nowMs);
  }

  if (hasStatus && millis() - lastStatusAtMs > 15000) {
    updateSignalState(AgentSignalState::Unknown, millis());
    clearSignalPage();
    usageCardDirty = true;
  }

  updateDisplay();
  delay(25);
}
