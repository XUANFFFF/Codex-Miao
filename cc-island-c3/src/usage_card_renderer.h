#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "display_types.h"

class UsageCardRenderer {
 public:
  explicit UsageCardRenderer(Adafruit_ST7789& tft);

  void renderFrame(const UsageData& data, bool fullRedraw);

 private:
  void drawStaticCard();
  void drawDynamicCard(const UsageData& data);
  void drawBoundedText(int16_t x, int16_t y, const char* text, size_t capacity, uint8_t textSize,
                       uint16_t color, int16_t clearWidth, bool rightAlign = false);
  void drawBar(int16_t x, int16_t y, int16_t width, int16_t height, int16_t radius, uint8_t pct,
               uint16_t color);
  void clearRect(int16_t x, int16_t y, int16_t w, int16_t h);

  Adafruit_ST7789& tft_;
};
