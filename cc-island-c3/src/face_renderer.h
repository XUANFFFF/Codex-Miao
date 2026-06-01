#pragma once

#include <stdint.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "display_types.h"

class FaceRenderer {
 public:
  static constexpr int16_t kEyeCanvasW = 68;
  static constexpr int16_t kEyeCanvasH = 84;
  static constexpr int16_t kLowerCanvasW = 168;
  static constexpr int16_t kLowerCanvasH = 56;

  explicit FaceRenderer(Adafruit_ST7789& tft);

  void begin();
  bool update(uint32_t nowMs);
  void render(bool fullRedraw);
  void setExpression(FaceExpression expression, bool hold = false);
  void clearExpressionHold();
  void setAutoCycleEnabled(bool enabled);
  bool autoCycleEnabled() const;
  const FaceState& state() const;

 private:
  void applyExpression(FaceExpression expression);
  void pickNextExpression(uint32_t nowMs);
  void updateBlink(uint32_t nowMs);
  void updateGaze(uint32_t nowMs);
  void updateBob(uint32_t nowMs);
  void drawEye(int16_t centerX, int16_t centerY, int16_t width, int16_t height, bool leftEye);
  void drawLowerFace();

  Adafruit_ST7789& tft_;
  GFXcanvas16 eyeCanvas_;
  GFXcanvas16 lowerCanvas_;
  FaceState state_;
  uint32_t nextBlinkAtMs_ = 0;
  uint32_t blinkEndsAtMs_ = 0;
  uint32_t nextGazeAtMs_ = 0;
  uint32_t lastExpressionAtMs_ = 0;
  bool autoCycleEnabled_ = true;
  bool expressionHoldActive_ = false;
};
