#include "signal_page_renderer.h"

namespace {
constexpr uint16_t kBg = ST77XX_BLACK;
constexpr uint16_t kLampCore = ST77XX_RED;
constexpr uint16_t kLampGlow = 0xC800;
constexpr uint16_t kLabel = ST77XX_WHITE;

constexpr int16_t kCenterX = 120;
constexpr int16_t kLampCenterY = 90;
constexpr int16_t kLampOuterR = 36;
constexpr int16_t kLampInnerR = 24;
constexpr int16_t kLabelY = 154;
}  // namespace

SignalPageRenderer::SignalPageRenderer(Adafruit_ST7789& tft) : tft_(tft) {}

void SignalPageRenderer::render(const SignalPageState& state, bool fullRedraw) {
  if (state.kind == SignalPageKind::None) {
    return;
  }

  (void)fullRedraw;

  const char* label = labelFor(state.kind);
  if (label == nullptr) {
    return;
  }

  tft_.fillScreen(kBg);
  drawSignalLamp();
  drawLabel(label);
}

const char* SignalPageRenderer::labelFor(SignalPageKind kind) const {
  switch (kind) {
    case SignalPageKind::Permission:
      return "NEED OK";
    case SignalPageKind::Blocked:
      return "BLOCKED";
    case SignalPageKind::None:
    default:
      return nullptr;
  }
}

void SignalPageRenderer::drawSignalLamp() {
  tft_.fillCircle(kCenterX, kLampCenterY, kLampOuterR, kLampGlow);
  tft_.fillCircle(kCenterX, kLampCenterY, kLampInnerR, kLampCore);
  tft_.fillCircle(kCenterX - 9, kLampCenterY - 9, 6, 0xFBEB);
}

void SignalPageRenderer::drawLabel(const char* label) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  tft_.setTextSize(2);
  tft_.getTextBounds(label, 0, kLabelY, &x1, &y1, &width, &height);
  tft_.setTextColor(kLabel, kBg);
  tft_.setCursor(kCenterX - static_cast<int16_t>(width / 2), kLabelY);
  tft_.print(label);
}
