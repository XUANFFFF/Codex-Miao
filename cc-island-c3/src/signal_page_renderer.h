#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "display_types.h"

class SignalPageRenderer {
 public:
  explicit SignalPageRenderer(Adafruit_ST7789& tft);

  void render(const SignalPageState& state, bool fullRedraw);

 private:
  const char* labelFor(SignalPageKind kind) const;
  void drawSignalLamp();
  void drawLabel(const char* label);

  Adafruit_ST7789& tft_;
};
