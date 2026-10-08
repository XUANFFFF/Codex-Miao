#include "face_renderer.h"

#include <Arduino.h>

namespace {
constexpr uint16_t kBg = ST77XX_BLACK;
constexpr uint16_t kEyeWhite = ST77XX_WHITE;
constexpr uint16_t kMouthWhite = ST77XX_WHITE;
constexpr uint16_t kAccentCyan = ST77XX_CYAN;
constexpr uint16_t kAccentPink = 0xF81F;
constexpr uint16_t kAccentWarm = 0xFD20;
constexpr uint16_t kBadgeLampGreen = 0x07E0;
constexpr uint16_t kBadgeLampYellow = 0xFE60;
constexpr uint16_t kBadgeLampRed = 0xF800;
constexpr int16_t kBadgeX = 180;
constexpr int16_t kBadgeY = 208;
constexpr int16_t kBadgeW = 60;
constexpr int16_t kBadgeH = 30;
constexpr int16_t kLampRadius = 7;
constexpr int16_t kEyeY = 96;
constexpr int16_t kLeftEyeX = 78;
constexpr int16_t kRightEyeX = 162;
constexpr int16_t kEyeWidth = 48;
constexpr int16_t kEyeHeight = 60;
constexpr int16_t kEyeCorner = 18;
constexpr int16_t kMinEyeHeight = 8;
constexpr int16_t kMinInternalEyeHeight = 22;
constexpr int16_t kBlushRadius = 8;
constexpr int16_t kLowerFaceCenterX = 120;
constexpr int16_t kLowerFaceCenterY = 160;
constexpr uint32_t kExpressionHoldMs = 600000;
constexpr uint32_t kBlinkInitialDelayMs = 36000;
constexpr uint32_t kBlinkBaseIntervalMs = 36000;
constexpr uint32_t kBlinkVarianceWindowMs = 34000;
constexpr FaceExpression kExpressionCycle[] = {
    FaceExpression::Neutral,
    FaceExpression::Happy,
    FaceExpression::Curious,
    FaceExpression::Sleepy,
    FaceExpression::AngryPout,
};

int16_t maxEyeHeight(int16_t value) {
  return value < kMinEyeHeight ? kMinEyeHeight : value;
}

int16_t minValue(int16_t a, int16_t b) {
  return a < b ? a : b;
}

bool faceStatesEqual(const FaceState& left, const FaceState& right) {
  return left.expression == right.expression && left.eyeOpen == right.eyeOpen &&
         left.gazeX == right.gazeX && left.gazeY == right.gazeY &&
         left.bobY == right.bobY && left.showBlush == right.showBlush &&
         left.accentColor == right.accentColor;
}

bool signalOverlayStatesEqual(const SignalOverlayState& left, const SignalOverlayState& right) {
  return left.visible == right.visible && left.signal == right.signal;
}

uint16_t activeLampColor(AgentSignalState signal) {
  switch (signal) {
    case AgentSignalState::Idle:
      return kBadgeLampGreen;
    case AgentSignalState::Thinking:
    case AgentSignalState::Working:
      return kBadgeLampYellow;
    case AgentSignalState::Permission:
    case AgentSignalState::Blocked:
      return kBadgeLampRed;
    default:
      return kBadgeLampGreen;
  }
}

}  // namespace

FaceRenderer::FaceRenderer(Adafruit_ST7789& tft)
    : tft_(tft),
      eyeCanvas_(FaceRenderer::kEyeCanvasW, FaceRenderer::kEyeCanvasH),
      lowerCanvas_(FaceRenderer::kLowerCanvasW, FaceRenderer::kLowerCanvasH) {}

void FaceRenderer::begin() {
  const uint32_t nowMs = millis();
  nextBlinkAtMs_ = nowMs + kBlinkInitialDelayMs;
  nextGazeAtMs_ = nowMs + 900;
  lastExpressionAtMs_ = nowMs;
  state_ = FaceState{};
  autoCycleEnabled_ = true;
  expressionHoldActive_ = false;
  signalBadgeDrawn_ = false;
  lastSignalOverlay_ = SignalOverlayState{};
  applyExpression(FaceExpression::Neutral);
}

bool FaceRenderer::update(uint32_t nowMs) {
  const FaceState previous = state_;
  pickNextExpression(nowMs);
  updateBlink(nowMs);
  updateGaze(nowMs);
  updateBob(nowMs);
  return !faceStatesEqual(previous, state_);
}

void FaceRenderer::render(bool fullRedraw, const SignalOverlayState& overlay) {
  if (fullRedraw) {
    tft_.fillScreen(kBg);
    signalBadgeDrawn_ = false;
  }

  drawEye(kLeftEyeX, kEyeY + static_cast<int16_t>(state_.bobY), kEyeWidth, kEyeHeight, true);
  drawEye(kRightEyeX, kEyeY + static_cast<int16_t>(state_.bobY), kEyeWidth, kEyeHeight, false);
  drawLowerFace();
  if (fullRedraw || !signalBadgeDrawn_ || !signalOverlayStatesEqual(lastSignalOverlay_, overlay)) {
    drawSignalBadge(overlay);
  }
}

void FaceRenderer::render(bool fullRedraw) {
  render(fullRedraw, SignalOverlayState{});
}

void FaceRenderer::setExpression(FaceExpression expression, bool hold) {
  applyExpression(expression);
  expressionHoldActive_ = hold;
  if (hold) {
    autoCycleEnabled_ = false;
  }
}

void FaceRenderer::clearExpressionHold() {
  expressionHoldActive_ = false;
}

void FaceRenderer::setAutoCycleEnabled(bool enabled) {
  autoCycleEnabled_ = enabled;
}

bool FaceRenderer::autoCycleEnabled() const {
  return autoCycleEnabled_;
}

const FaceState& FaceRenderer::state() const {
  return state_;
}

void FaceRenderer::applyExpression(FaceExpression expression) {
  state_.expression = expression;
  state_.showBlush = expression == FaceExpression::Happy;
  state_.accentColor = expression == FaceExpression::AngryPout ? ST77XX_RED : kAccentCyan;
}

void FaceRenderer::pickNextExpression(uint32_t nowMs) {
  if (expressionHoldActive_ || !autoCycleEnabled_) {
    return;
  }

  if (nowMs - lastExpressionAtMs_ < kExpressionHoldMs) {
    return;
  }

  const size_t phase =
      (nowMs / kExpressionHoldMs) % (sizeof(kExpressionCycle) / sizeof(kExpressionCycle[0]));
  applyExpression(kExpressionCycle[phase]);
  lastExpressionAtMs_ = nowMs;
}

void FaceRenderer::updateBlink(uint32_t nowMs) {
  if (blinkEndsAtMs_ != 0 && static_cast<int32_t>(blinkEndsAtMs_ - nowMs) > 0) {
    state_.eyeOpen = 0.18f;
    return;
  }

  if (nextBlinkAtMs_ != 0 && static_cast<int32_t>(nextBlinkAtMs_ - nowMs) <= 0) {
    blinkEndsAtMs_ = nowMs + 140;
    nextBlinkAtMs_ = nowMs + kBlinkBaseIntervalMs + (nowMs % kBlinkVarianceWindowMs);
    state_.eyeOpen = 0.18f;
    return;
  }

  blinkEndsAtMs_ = 0;
  state_.eyeOpen = state_.expression == FaceExpression::Sleepy ? 0.45f : 1.0f;
}

void FaceRenderer::updateGaze(uint32_t nowMs) {
  if (static_cast<int32_t>(nextGazeAtMs_ - nowMs) > 0) {
    return;
  }

  const int8_t xPattern[5] = {0, 5, -6, 3, -2};
  const int8_t yPattern[5] = {0, -2, 1, -1, 2};
  const uint8_t idx = static_cast<uint8_t>((nowMs / 900U) % 5U);
  state_.gazeX = static_cast<float>(xPattern[idx]);
  state_.gazeY = static_cast<float>(yPattern[idx]);
  nextGazeAtMs_ = nowMs + 900;
}

void FaceRenderer::updateBob(uint32_t nowMs) {
  state_.bobY = static_cast<float>((nowMs / 280U) % 3U) - 1.0f;
}

void FaceRenderer::drawEye(int16_t centerX, int16_t centerY, int16_t width, int16_t height,
                           bool leftEye) {
  eyeCanvas_.fillScreen(kBg);

  const int16_t canvasX = centerX - (FaceRenderer::kEyeCanvasW / 2);
  const int16_t canvasY = centerY - (FaceRenderer::kEyeCanvasH / 2);
  const int16_t eyeHeight = maxEyeHeight(static_cast<int16_t>(height * state_.eyeOpen));
  const int16_t eyeX = (FaceRenderer::kEyeCanvasW - width) / 2;
  const int16_t eyeY = (FaceRenderer::kEyeCanvasH - eyeHeight) / 2;
  const int16_t cornerRadius = minValue(kEyeCorner, minValue(width / 2, eyeHeight / 2));

  eyeCanvas_.fillRoundRect(eyeX, eyeY, width, eyeHeight, cornerRadius, kEyeWhite);

  if (eyeHeight < kMinInternalEyeHeight) {
    tft_.drawRGBBitmap(canvasX, canvasY, eyeCanvas_.getBuffer(), FaceRenderer::kEyeCanvasW,
                       FaceRenderer::kEyeCanvasH);
    return;
  }

  const int16_t pupilR = state_.expression == FaceExpression::Curious ? 10 : 8;
  const int16_t eyeInteriorHalfHeight = eyeHeight / 2 - pupilR - 2;
  if (eyeInteriorHalfHeight <= 0) {
    tft_.drawRGBBitmap(canvasX, canvasY, eyeCanvas_.getBuffer(), FaceRenderer::kEyeCanvasW,
                       FaceRenderer::kEyeCanvasH);
    return;
  }

  int16_t pupilX = (FaceRenderer::kEyeCanvasW / 2) + static_cast<int16_t>(state_.gazeX);
  int16_t pupilY = (FaceRenderer::kEyeCanvasH / 2) + static_cast<int16_t>(state_.gazeY);
  const int16_t pupilMinX = eyeX + pupilR + 4;
  const int16_t pupilMaxX = eyeX + width - pupilR - 4;
  if (pupilX < pupilMinX) {
    pupilX = pupilMinX;
  } else if (pupilX > pupilMaxX) {
    pupilX = pupilMaxX;
  }

  const int16_t pupilCenterY = FaceRenderer::kEyeCanvasH / 2;
  const int16_t pupilMinY = pupilCenterY - eyeInteriorHalfHeight;
  const int16_t pupilMaxY = pupilCenterY + eyeInteriorHalfHeight;
  if (pupilY < pupilMinY) {
    pupilY = pupilMinY;
  } else if (pupilY > pupilMaxY) {
    pupilY = pupilMaxY;
  }

  eyeCanvas_.fillCircle(pupilX, pupilY, pupilR, kBg);

  if (eyeHeight < (kMinInternalEyeHeight + 6)) {
    tft_.drawRGBBitmap(canvasX, canvasY, eyeCanvas_.getBuffer(), FaceRenderer::kEyeCanvasW,
                       FaceRenderer::kEyeCanvasH);
    return;
  }

  const int16_t highlightX = pupilX + (leftEye ? 5 : -5);
  const int16_t highlightY = pupilY - 5;
  const int16_t highlightMinY = eyeY + 5;
  if (highlightY >= highlightMinY) {
    eyeCanvas_.fillCircle(highlightX, highlightY, 3, state_.accentColor);
  }

  if (state_.expression == FaceExpression::AngryPout) {
    const int16_t browY = eyeY - 7;
    if (leftEye) {
      eyeCanvas_.drawLine(eyeX + 4, browY + 3, eyeX + width - 2, browY, state_.accentColor);
    } else {
      eyeCanvas_.drawLine(eyeX + 2, browY, eyeX + width - 4, browY + 3, state_.accentColor);
    }
  } else if (state_.expression == FaceExpression::Sleepy) {
    eyeCanvas_.drawFastHLine(eyeX + 6, eyeY + 4, width - 12, kAccentWarm);
  }

  tft_.drawRGBBitmap(canvasX, canvasY, eyeCanvas_.getBuffer(), FaceRenderer::kEyeCanvasW,
                     FaceRenderer::kEyeCanvasH);
}

void FaceRenderer::drawLowerFace() {
  lowerCanvas_.fillScreen(kBg);
  const int16_t canvasX = kLowerFaceCenterX - (FaceRenderer::kLowerCanvasW / 2);
  const int16_t canvasY = kLowerFaceCenterY - (FaceRenderer::kLowerCanvasH / 2);
  const int16_t mouthY = (FaceRenderer::kLowerCanvasH / 2) + static_cast<int16_t>(state_.bobY);

  switch (state_.expression) {
    case FaceExpression::Happy:
      lowerCanvas_.drawRoundRect(72, mouthY, 48, 10, 5, kMouthWhite);
      lowerCanvas_.drawPixel(76, mouthY + 4, kMouthWhite);
      lowerCanvas_.drawPixel(116, mouthY + 4, kMouthWhite);
      break;
    case FaceExpression::Curious:
      lowerCanvas_.drawCircle(FaceRenderer::kLowerCanvasW / 2, mouthY + 5, 5, kMouthWhite);
      break;
    case FaceExpression::Sleepy:
      lowerCanvas_.drawFastHLine(78, mouthY + 4, 36, kMouthWhite);
      break;
    case FaceExpression::AngryPout:
      lowerCanvas_.drawLine(76, mouthY + 8, 96, mouthY + 12, kMouthWhite);
      lowerCanvas_.drawLine(96, mouthY + 12, 116, mouthY + 8, kMouthWhite);
      break;
    case FaceExpression::Neutral:
    default:
      lowerCanvas_.drawFastHLine(78, mouthY + 6, 36, kMouthWhite);
      break;
  }

  if (state_.showBlush) {
    const int16_t blushY = 13 + static_cast<int16_t>(state_.bobY);
    lowerCanvas_.fillCircle(16, blushY, kBlushRadius, kAccentPink);
    lowerCanvas_.fillCircle(FaceRenderer::kLowerCanvasW - 16, blushY, kBlushRadius,
                            kAccentPink);
  }

  tft_.drawRGBBitmap(canvasX, canvasY, lowerCanvas_.getBuffer(), FaceRenderer::kLowerCanvasW,
                     FaceRenderer::kLowerCanvasH);
}

void FaceRenderer::drawSignalBadge(const SignalOverlayState& overlay) {
  tft_.fillRect(kBadgeX, kBadgeY, kBadgeW, kBadgeH, kBg);
  lastSignalOverlay_ = overlay;
  signalBadgeDrawn_ = true;
  if (!overlay.visible) {
    return;
  }

  const uint16_t activeColor = activeLampColor(overlay.signal);
  const int16_t lampX = kBadgeX + (kBadgeW / 2);
  const int16_t lampY = kBadgeY + (kBadgeH / 2);
  tft_.fillCircle(lampX, lampY, kLampRadius, activeColor);
}
