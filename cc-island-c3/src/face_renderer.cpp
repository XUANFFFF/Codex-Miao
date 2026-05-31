#include "face_renderer.h"

#include <Arduino.h>

namespace {
constexpr uint16_t kBg = ST77XX_BLACK;
constexpr uint16_t kEyeWhite = ST77XX_WHITE;
constexpr uint16_t kMouthWhite = ST77XX_WHITE;
constexpr uint16_t kAccentCyan = ST77XX_CYAN;
constexpr uint16_t kAccentPink = 0xF81F;
constexpr uint16_t kAccentWarm = 0xFD20;
constexpr int16_t kFaceBoundsX = 28;
constexpr int16_t kFaceBoundsY = 42;
constexpr int16_t kFaceBoundsW = 184;
constexpr int16_t kFaceBoundsH = 142;
constexpr int16_t kEyeY = 96;
constexpr int16_t kLeftEyeX = 78;
constexpr int16_t kRightEyeX = 162;
constexpr int16_t kEyeWidth = 48;
constexpr int16_t kEyeHeight = 60;
constexpr int16_t kEyeCorner = 18;
constexpr int16_t kMinEyeHeight = 8;
constexpr int16_t kMinInternalEyeHeight = 22;
constexpr uint32_t kExpressionHoldMs = 5000;
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
}  // namespace

FaceRenderer::FaceRenderer(Adafruit_ST7789& tft) : tft_(tft) {}

void FaceRenderer::begin() {
  const uint32_t nowMs = millis();
  nextBlinkAtMs_ = nowMs + 1800;
  nextGazeAtMs_ = nowMs + 900;
  lastExpressionAtMs_ = nowMs;
  state_ = FaceState{};
  autoCycleEnabled_ = true;
  expressionHoldActive_ = false;
  applyExpression(FaceExpression::Neutral);
}

void FaceRenderer::update(uint32_t nowMs) {
  pickNextExpression(nowMs);
  updateBlink(nowMs);
  updateGaze(nowMs);
  updateBob(nowMs);
}

void FaceRenderer::render(bool fullRedraw) {
  if (fullRedraw) {
    tft_.fillScreen(kBg);
  }

  tft_.fillRect(kFaceBoundsX, kFaceBoundsY, kFaceBoundsW, kFaceBoundsH, kBg);
  drawEye(kLeftEyeX, kEyeY + static_cast<int16_t>(state_.bobY), kEyeWidth, kEyeHeight, true);
  drawEye(kRightEyeX, kEyeY + static_cast<int16_t>(state_.bobY), kEyeWidth, kEyeHeight, false);
  drawMouth();

  if (state_.showBlush) {
    tft_.fillCircle(52, 145 + static_cast<int16_t>(state_.bobY), 8, kAccentPink);
    tft_.fillCircle(188, 145 + static_cast<int16_t>(state_.bobY), 8, kAccentPink);
  }
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

  const size_t phase = (nowMs / kExpressionHoldMs) % (sizeof(kExpressionCycle) / sizeof(kExpressionCycle[0]));
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
    nextBlinkAtMs_ = nowMs + 1800 + (nowMs % 1700);
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

void FaceRenderer::drawEye(int16_t centerX, int16_t centerY, int16_t width, int16_t height, bool leftEye) {
  const int16_t eyeHeight = maxEyeHeight(static_cast<int16_t>(height * state_.eyeOpen));
  const int16_t eyeX = centerX - width / 2;
  const int16_t eyeY = centerY - eyeHeight / 2;
  const int16_t cornerRadius = minValue(kEyeCorner, minValue(width / 2, eyeHeight / 2));

  tft_.fillRoundRect(eyeX, eyeY, width, eyeHeight, cornerRadius, kEyeWhite);

  if (eyeHeight < kMinInternalEyeHeight) {
    return;
  }

  const int16_t pupilR = state_.expression == FaceExpression::Curious ? 10 : 8;
  const int16_t eyeInteriorHalfHeight = eyeHeight / 2 - pupilR - 2;
  if (eyeInteriorHalfHeight <= 0) {
    return;
  }

  int16_t pupilX = centerX + static_cast<int16_t>(state_.gazeX);
  int16_t pupilY = centerY + static_cast<int16_t>(state_.gazeY);
  const int16_t pupilMinX = eyeX + pupilR + 4;
  const int16_t pupilMaxX = eyeX + width - pupilR - 4;
  if (pupilX < pupilMinX) {
    pupilX = pupilMinX;
  } else if (pupilX > pupilMaxX) {
    pupilX = pupilMaxX;
  }

  const int16_t pupilMinY = centerY - eyeInteriorHalfHeight;
  const int16_t pupilMaxY = centerY + eyeInteriorHalfHeight;
  if (pupilY < pupilMinY) {
    pupilY = pupilMinY;
  } else if (pupilY > pupilMaxY) {
    pupilY = pupilMaxY;
  }

  tft_.fillCircle(pupilX, pupilY, pupilR, kBg);

  if (eyeHeight < (kMinInternalEyeHeight + 6)) {
    return;
  }

  const int16_t highlightX = pupilX + (leftEye ? 5 : -5);
  const int16_t highlightY = pupilY - 5;
  const int16_t highlightMinY = eyeY + 5;
  if (highlightY >= highlightMinY) {
    tft_.fillCircle(highlightX, highlightY, 3, state_.accentColor);
  }

  if (state_.expression == FaceExpression::AngryPout) {
    const int16_t browY = eyeY - 7;
    if (leftEye) {
      tft_.drawLine(eyeX + 4, browY + 3, eyeX + width - 2, browY, state_.accentColor);
    } else {
      tft_.drawLine(eyeX + 2, browY, eyeX + width - 4, browY + 3, state_.accentColor);
    }
  } else if (state_.expression == FaceExpression::Sleepy) {
    tft_.drawFastHLine(eyeX + 6, eyeY + 4, width - 12, kAccentWarm);
  }
}

void FaceRenderer::drawMouth() {
  const int16_t mouthY = 160 + static_cast<int16_t>(state_.bobY);
  tft_.fillRect(92, 150, 56, 24, kBg);

  switch (state_.expression) {
    case FaceExpression::Happy:
      tft_.drawRoundRect(96, mouthY, 48, 10, 5, kMouthWhite);
      tft_.drawPixel(100, mouthY + 4, kMouthWhite);
      tft_.drawPixel(140, mouthY + 4, kMouthWhite);
      break;
    case FaceExpression::Curious:
      tft_.drawCircle(120, mouthY + 5, 5, kMouthWhite);
      break;
    case FaceExpression::Sleepy:
      tft_.drawFastHLine(102, mouthY + 4, 36, kMouthWhite);
      break;
    case FaceExpression::AngryPout:
      tft_.drawLine(100, mouthY + 8, 120, mouthY + 12, kMouthWhite);
      tft_.drawLine(120, mouthY + 12, 140, mouthY + 8, kMouthWhite);
      break;
    case FaceExpression::Neutral:
    default:
      tft_.drawFastHLine(102, mouthY + 6, 36, kMouthWhite);
      break;
  }
}
