#include "usage_card_renderer.h"

#include <stdio.h>

namespace {
constexpr uint16_t kBg = ST77XX_BLACK;
constexpr uint16_t kFrame = ST77XX_WHITE;
constexpr uint16_t kAccent = ST77XX_CYAN;
constexpr uint16_t kMuted = ST77XX_BLUE;
constexpr uint16_t kWeekAccent = ST77XX_MAGENTA;
constexpr uint16_t kBadgeLampGreen = 0x07E0;
constexpr uint16_t kBadgeLampYellow = 0xFE60;
constexpr uint16_t kBadgeLampRed = 0xF800;

constexpr int16_t kCardX = 16;
constexpr int16_t kCardY = 2;
constexpr int16_t kCardW = 208;
constexpr int16_t kBadgeX = 174;
constexpr int16_t kBadgeY = 208;
constexpr int16_t kBadgeW = 60;
constexpr int16_t kBadgeH = 30;
constexpr int16_t kLampRadius = 7;

constexpr int16_t kWindowBarW = kCardW - 38;
constexpr int16_t kWeekBarW = kCardW - 38;
constexpr int16_t kPercentBoxX = kCardX + kCardW - 118;
constexpr int16_t kPercentBoxW = 110;
constexpr int16_t kWeekPctBoxX = kCardX + kCardW - 76;
constexpr int16_t kWeekPctBoxW = 62;
constexpr int16_t kResetTextX = kCardX + 92;
constexpr int16_t kResetTextY = kCardY + 192;
constexpr int16_t kResetTextW = 60;
constexpr int16_t kSyncTextX = kCardX + 154;
constexpr int16_t kSyncTextY = kCardY + 196;
constexpr int16_t kSyncTextW = 50;

uint8_t clampPct(uint8_t value) {
  return value > 100 ? 100 : value;
}

size_t boundedTextLength(const char* text, size_t capacity) {
  if (text == nullptr) {
    return 0;
  }

  size_t length = 0;
  while (length < capacity && text[length] != '\0') {
    ++length;
  }
  return length;
}

void copyBoundedText(char* dest, size_t destCapacity, const char* src, size_t srcCapacity) {
  if (destCapacity == 0) {
    return;
  }

  const size_t srcLength = boundedTextLength(src, srcCapacity);
  const size_t copyLength = srcLength < (destCapacity - 1) ? srcLength : (destCapacity - 1);
  for (size_t i = 0; i < copyLength; ++i) {
    dest[i] = src[i];
  }
  dest[copyLength] = '\0';
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

UsageCardRenderer::UsageCardRenderer(Adafruit_ST7789& tft) : tft_(tft) {}

void UsageCardRenderer::renderFrame(const UsageData& data, bool fullRedraw,
                                    const SignalOverlayState& overlay) {
  if (fullRedraw) {
    tft_.fillScreen(kBg);
    drawStaticCard();
    signalBadgeDrawn_ = false;
  }

  drawDynamicCard(data);
  if (fullRedraw || !signalBadgeDrawn_ || !signalOverlayStatesEqual(lastSignalOverlay_, overlay)) {
    drawSignalBadge(overlay);
  }
}

void UsageCardRenderer::renderFrame(const UsageData& data, bool fullRedraw) {
  renderFrame(data, fullRedraw, SignalOverlayState{});
}

void UsageCardRenderer::drawStaticCard() {
  tft_.setTextColor(kAccent, kBg);
  tft_.setTextSize(2);
  tft_.setCursor(kCardX + 14, kCardY + 12);
  tft_.print("Codex");

  tft_.setTextColor(kMuted, kBg);
  tft_.setTextSize(1);
  tft_.setCursor(kCardX + 16, kCardY + 32);
  tft_.print("remaining");

  tft_.setTextColor(kFrame, kBg);
  tft_.setTextSize(2);
  tft_.setCursor(kCardX + 16, kCardY + 62);
  tft_.print("5h");

  tft_.drawRoundRect(kCardX + 16, kCardY + 82, kCardW - 32, 32, 10, kMuted);

  tft_.setTextColor(kFrame, kBg);
  tft_.setTextSize(2);
  tft_.setCursor(kCardX + 16, kCardY + 134);
  tft_.print("7d");

  tft_.drawRoundRect(kCardX + 16, kCardY + 154, kCardW - 32, 22, 9, kMuted);

  tft_.setTextColor(kMuted, kBg);
  tft_.setTextSize(2);
  tft_.setCursor(kCardX + 16, kCardY + 192);
  tft_.print("Reset");
}

void UsageCardRenderer::drawDynamicCard(const UsageData& data) {
  const uint8_t windowPct = data.usageAvailable ? clampPct(data.windowPct) : 0;
  const uint8_t weekPct = data.usageAvailable ? clampPct(data.weekPct) : 0;
  char pctText[5];
  char weekPctText[5];

  if (data.usageAvailable) {
    snprintf(pctText, sizeof(pctText), "%u%%", static_cast<unsigned>(windowPct));
    snprintf(weekPctText, sizeof(weekPctText), "%u%%", static_cast<unsigned>(weekPct));
  } else {
    snprintf(pctText, sizeof(pctText), "--");
    snprintf(weekPctText, sizeof(weekPctText), "--");
  }

  drawBoundedText(kPercentBoxX, kCardY + 10, pctText, sizeof(pctText), 5, kFrame, kPercentBoxW,
                  true);

  clearRect(kCardX + 19, kCardY + 85, kWindowBarW, 26);
  drawBar(kCardX + 19, kCardY + 85, kWindowBarW, 26, 8, windowPct, kAccent);

  clearRect(kCardX + 19, kCardY + 157, kWeekBarW, 16);
  drawBar(kCardX + 19, kCardY + 157, kWeekBarW, 16, 7, weekPct, kWeekAccent);
  drawBoundedText(kWeekPctBoxX + 4, kCardY + 134, weekPctText, sizeof(weekPctText), 2, kFrame,
                  kWeekPctBoxW - 4);

  const char* resetText = data.usageAvailable ? data.resetText : "--:--";
  drawBoundedText(kResetTextX, kResetTextY, resetText, kResetTextCapacity, 2, kFrame, kResetTextW);
  drawBoundedText(kSyncTextX, kSyncTextY, data.syncText, kSyncTextCapacity, 1, kMuted,
                  kSyncTextW);
}

void UsageCardRenderer::drawSignalBadge(const SignalOverlayState& overlay) {
  clearRect(kBadgeX, kBadgeY, kBadgeW, kBadgeH);
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

void UsageCardRenderer::drawBoundedText(int16_t x, int16_t y, const char* text, size_t capacity,
                                        uint8_t textSize, uint16_t color, int16_t clearWidth,
                                        bool rightAlign) {
  if (clearWidth <= 0) {
    return;
  }

  char buffer[kSyncTextCapacity];
  copyBoundedText(buffer, sizeof(buffer), text, capacity);

  size_t length = boundedTextLength(buffer, sizeof(buffer));
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  tft_.setTextSize(textSize);
  while (length > 0) {
    buffer[length] = '\0';
    tft_.getTextBounds(buffer, x, y, &x1, &y1, &width, &height);
    if (width <= static_cast<uint16_t>(clearWidth)) {
      break;
    }
    --length;
  }

  clearRect(x, y - 2, clearWidth, static_cast<int16_t>(textSize * 8 + 4));
  tft_.setTextColor(color, kBg);

  int16_t drawX = x;
  if (rightAlign && width < static_cast<uint16_t>(clearWidth)) {
    drawX += clearWidth - static_cast<int16_t>(width);
  }

  tft_.setCursor(drawX, y);
  for (size_t i = 0; i < length; ++i) {
    tft_.write(static_cast<uint8_t>(buffer[i]));
  }
}

void UsageCardRenderer::drawBar(int16_t x, int16_t y, int16_t width, int16_t height, int16_t radius,
                                uint8_t pct, uint16_t color) {
  const int16_t fillWidth = width * pct / 100;
  if (fillWidth <= 0) {
    return;
  }

  tft_.fillRoundRect(x, y, fillWidth, height, radius, color);
}

void UsageCardRenderer::clearRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  tft_.fillRect(x, y, w, h, kBg);
}
