#pragma once

#include <stddef.h>
#include <stdint.h>

enum class FaceExpression : uint8_t {
  Neutral,
  Happy,
  Curious,
  Sleepy,
  AngryPout,
};

enum class ScreenMode : uint8_t {
  Face,
  UsageCard,
};

enum class TransitionDirection : uint8_t {
  None,
  FaceToCard,
  CardToFace,
};

constexpr size_t kResetTextCapacity = 6;
constexpr size_t kSyncTextCapacity = 16;

struct UsageData {
  uint8_t windowPct = 41;
  uint8_t weekPct = 17;
  uint16_t resetMin = 133;
  char resetText[kResetTextCapacity] = "--:--";
  char syncText[kSyncTextCapacity] = "waiting";
};

struct FaceState {
  FaceExpression expression = FaceExpression::Neutral;
  float eyeOpen = 1.0f;
  float gazeX = 0.0f;
  float gazeY = 0.0f;
  float bobY = 0.0f;
  bool showBlush = false;
  uint16_t accentColor = 0;
};

struct TransitionState {
  TransitionDirection direction = TransitionDirection::None;
  bool active = false;
  uint32_t startedAtMs = 0;
  uint16_t durationMs = 320;
  uint8_t progress = 0;
};
