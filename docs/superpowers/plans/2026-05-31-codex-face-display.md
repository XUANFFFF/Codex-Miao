# Codex Face Display Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a dual-mode ESP32 display that shows a StackChan-inspired idle face by default, switches to the existing Codex usage card when usage values change, keeps that card alive for 3 minutes after each meaningful update, and then returns to the animated face with a smooth transition.

**Architecture:** Keep Wi-Fi, serial input, HTTP fetch, and JSON parsing flow centered in `main.cpp`, but move display behavior into focused modules under `src/`. Add a usage activity tracker, a screen mode controller with lightweight transitions, and a face renderer/animator so the firmware can switch between idle character mode and the existing usage card without tangling rendering and data logic.

**Tech Stack:** PlatformIO, Arduino framework for ESP32-C3, `Adafruit_ST7789`, `Adafruit_GFX`, `ArduinoJson`

---

## Planned File Structure

- Modify: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\main.cpp`
  Responsibility: keep boot/setup, connectivity, JSON ingest, and top-level loop orchestration while wiring in the new modules.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\display_types.h`
  Responsibility: shared structs and enums for usage data, face expressions, screen modes, and transition metadata.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_state_tracker.h`
  Responsibility: compare incoming usage payloads, detect meaningful changes, and manage the 3-minute active deadline.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_state_tracker.cpp`
  Responsibility: implement the tracker logic with no rendering knowledge.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\screen_mode_controller.h`
  Responsibility: own current mode, target mode, transition state, and animation timing.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\screen_mode_controller.cpp`
  Responsibility: implement mode switching and stepped transition progress.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\face_renderer.h`
  Responsibility: declare face state, animator API, and renderer API.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\face_renderer.cpp`
  Responsibility: render the geometric face, eye animation, and idle expressions.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_card_renderer.h`
  Responsibility: wrap the current card drawing code behind a small interface.
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_card_renderer.cpp`
  Responsibility: move the current static/dynamic card rendering logic out of `main.cpp`.

### Task 1: Add Shared Display Types

**Files:**
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\display_types.h`

- [ ] **Step 1: Create the shared type header**

```cpp
#pragma once

#include <Arduino.h>
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

struct UsageData {
  uint8_t windowPct = 41;
  uint8_t weekPct = 17;
  uint16_t resetMin = 133;
  String resetText = "--:--";
  String syncText = "waiting";
};

struct FaceState {
  FaceExpression expression = FaceExpression::Neutral;
  float eyeOpen = 1.0f;
  float gazeX = 0.0f;
  float gazeY = 0.0f;
  float bobY = 0.0f;
  bool showBlush = false;
  uint16_t accentColor = 0x07FF;
};

struct TransitionState {
  TransitionDirection direction = TransitionDirection::None;
  bool active = false;
  uint32_t startedAtMs = 0;
  uint16_t durationMs = 320;
  uint8_t progress = 255;
};
```

- [ ] **Step 2: Verify the header matches the approved spec**

Check that the header includes:
- 5 face expressions
- 2 visible screen modes
- transition metadata
- usage data payload

Expected: all design-required concepts appear exactly once in a shared header.

- [ ] **Step 3: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/display_types.h
git commit -m "feat: add shared display types"
```

### Task 2: Build the Usage Activity Tracker

**Files:**
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_state_tracker.h`
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_state_tracker.cpp`

- [ ] **Step 1: Create the tracker header**

```cpp
#pragma once

#include "display_types.h"

class UsageStateTracker {
 public:
  static constexpr uint32_t kActiveWindowMs = 180000;

  UsageStateTracker();

  bool applyIncomingData(const UsageData& incoming, uint32_t nowMs);
  const UsageData& data() const;
  bool isUsageActive(uint32_t nowMs) const;
  uint32_t activeUntilMs() const;

 private:
  UsageData current_;
  uint32_t activeUntilMs_ = 0;
};
```

- [ ] **Step 2: Implement the tracker source**

```cpp
#include "usage_state_tracker.h"

UsageStateTracker::UsageStateTracker() = default;

bool UsageStateTracker::applyIncomingData(const UsageData& incoming, uint32_t nowMs) {
  const bool changed =
      incoming.windowPct != current_.windowPct ||
      incoming.weekPct != current_.weekPct ||
      incoming.resetText != current_.resetText;

  current_.windowPct = incoming.windowPct;
  current_.weekPct = incoming.weekPct;
  current_.resetMin = incoming.resetMin;
  current_.resetText = incoming.resetText;
  current_.syncText = incoming.syncText;

  if (changed) {
    activeUntilMs_ = nowMs + kActiveWindowMs;
  }

  return changed;
}

const UsageData& UsageStateTracker::data() const {
  return current_;
}

bool UsageStateTracker::isUsageActive(uint32_t nowMs) const {
  return activeUntilMs_ != 0 && static_cast<int32_t>(activeUntilMs_ - nowMs) > 0;
}

uint32_t UsageStateTracker::activeUntilMs() const {
  return activeUntilMs_;
}
```

- [ ] **Step 3: Sanity-check the rule against the spec**

Confirm the implementation:
- treats only `windowPct`, `weekPct`, and `resetText` as meaningful triggers
- still updates `resetMin` and `syncText`
- renews the 3-minute window only on meaningful changes

Expected: behavior matches the mixed strategy exactly.

- [ ] **Step 4: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/usage_state_tracker.h D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/usage_state_tracker.cpp
git commit -m "feat: add usage activity tracker"
```

### Task 3: Add the Screen Mode Controller

**Files:**
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\screen_mode_controller.h`
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\screen_mode_controller.cpp`

- [ ] **Step 1: Create the controller header**

```cpp
#pragma once

#include "display_types.h"

class ScreenModeController {
 public:
  ScreenModeController();

  void setMode(ScreenMode nextMode, uint32_t nowMs);
  void update(uint32_t nowMs);

  ScreenMode currentMode() const;
  ScreenMode targetMode() const;
  const TransitionState& transition() const;
  bool needsFullRedraw() const;
  void consumeFullRedrawFlag();

 private:
  ScreenMode currentMode_ = ScreenMode::Face;
  ScreenMode targetMode_ = ScreenMode::Face;
  TransitionState transition_;
  bool needsFullRedraw_ = true;
};
```

- [ ] **Step 2: Implement the controller source**

```cpp
#include "screen_mode_controller.h"

namespace {
uint8_t computeProgress(uint32_t nowMs, const TransitionState& state) {
  if (!state.active || state.durationMs == 0) {
    return 255;
  }
  const uint32_t elapsed = nowMs - state.startedAtMs;
  if (elapsed >= state.durationMs) {
    return 255;
  }
  return static_cast<uint8_t>((elapsed * 255U) / state.durationMs);
}
}  // namespace

ScreenModeController::ScreenModeController() = default;

void ScreenModeController::setMode(ScreenMode nextMode, uint32_t nowMs) {
  if (nextMode == targetMode_ && (!transition_.active || nextMode == currentMode_)) {
    return;
  }

  targetMode_ = nextMode;
  transition_.active = currentMode_ != targetMode_;
  transition_.startedAtMs = nowMs;
  transition_.durationMs = 320;
  transition_.progress = 0;
  transition_.direction = targetMode_ == ScreenMode::UsageCard
                              ? TransitionDirection::FaceToCard
                              : TransitionDirection::CardToFace;
  needsFullRedraw_ = true;
}

void ScreenModeController::update(uint32_t nowMs) {
  if (!transition_.active) {
    return;
  }

  transition_.progress = computeProgress(nowMs, transition_);
  if (transition_.progress >= 255) {
    transition_.active = false;
    currentMode_ = targetMode_;
    transition_.direction = TransitionDirection::None;
    needsFullRedraw_ = true;
  }
}

ScreenMode ScreenModeController::currentMode() const {
  return currentMode_;
}

ScreenMode ScreenModeController::targetMode() const {
  return targetMode_;
}

const TransitionState& ScreenModeController::transition() const {
  return transition_;
}

bool ScreenModeController::needsFullRedraw() const {
  return needsFullRedraw_;
}

void ScreenModeController::consumeFullRedrawFlag() {
  needsFullRedraw_ = false;
}
```

- [ ] **Step 3: Verify transition semantics**

Confirm:
- default mode is `Face`
- switching to usage card produces `FaceToCard`
- switching back produces `CardToFace`
- completed transitions set `currentMode_` to `targetMode_`

Expected: the controller can drive rendering without guessing state.

- [ ] **Step 4: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/screen_mode_controller.h D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/screen_mode_controller.cpp
git commit -m "feat: add screen mode controller"
```

### Task 4: Build the Face Renderer and Animator

**Files:**
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\face_renderer.h`
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\face_renderer.cpp`

- [ ] **Step 1: Create the face renderer header**

```cpp
#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "display_types.h"

class FaceRenderer {
 public:
  explicit FaceRenderer(Adafruit_ST7789& tft);

  void begin();
  void update(uint32_t nowMs);
  void render(bool fullRedraw);
  const FaceState& state() const;

 private:
  void pickNextExpression(uint32_t nowMs);
  void updateBlink(uint32_t nowMs);
  void updateGaze(uint32_t nowMs);
  void updateBob(uint32_t nowMs);
  void drawEye(int16_t centerX, int16_t centerY, int16_t width, int16_t height, bool leftEye);
  void drawMouth();

  Adafruit_ST7789& tft_;
  FaceState state_;
  uint32_t nextBlinkAtMs_ = 0;
  uint32_t blinkEndsAtMs_ = 0;
  uint32_t nextGazeAtMs_ = 0;
  uint32_t lastExpressionAtMs_ = 0;
};
```

- [ ] **Step 2: Implement the face renderer source**

```cpp
#include "face_renderer.h"

namespace {
constexpr uint16_t kBg = ST77XX_BLACK;
constexpr uint16_t kEyeWhite = ST77XX_WHITE;
constexpr uint16_t kAccentCyan = ST77XX_CYAN;
constexpr uint16_t kAccentPink = 0xF81F;
constexpr int16_t kEyeY = 96;
constexpr int16_t kLeftEyeX = 78;
constexpr int16_t kRightEyeX = 162;
constexpr int16_t kEyeWidth = 48;
constexpr int16_t kEyeHeight = 60;
}

FaceRenderer::FaceRenderer(Adafruit_ST7789& tft) : tft_(tft) {}

void FaceRenderer::begin() {
  nextBlinkAtMs_ = millis() + 1800;
  nextGazeAtMs_ = millis() + 900;
  lastExpressionAtMs_ = millis();
  state_.expression = FaceExpression::Neutral;
  state_.accentColor = kAccentCyan;
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

  tft_.fillRect(32, 52, 176, 132, kBg);
  drawEye(kLeftEyeX, kEyeY + static_cast<int16_t>(state_.bobY), kEyeWidth, kEyeHeight, true);
  drawEye(kRightEyeX, kEyeY + static_cast<int16_t>(state_.bobY), kEyeWidth, kEyeHeight, false);
  drawMouth();

  if (state_.showBlush) {
    tft_.fillCircle(48, 146, 8, kAccentPink);
    tft_.fillCircle(192, 146, 8, kAccentPink);
  }
}

const FaceState& FaceRenderer::state() const {
  return state_;
}

void FaceRenderer::pickNextExpression(uint32_t nowMs) {
  if (nowMs - lastExpressionAtMs_ < 5000) {
    return;
  }

  const uint8_t phase = (nowMs / 5000U) % 5U;
  state_.expression = static_cast<FaceExpression>(phase);
  state_.showBlush = state_.expression == FaceExpression::Happy;
  state_.accentColor = state_.expression == FaceExpression::AngryPout ? ST77XX_RED : kAccentCyan;
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
  const uint8_t idx = (nowMs / 900U) % 5U;
  state_.gazeX = xPattern[idx];
  state_.gazeY = yPattern[idx];
  nextGazeAtMs_ = nowMs + 900;
}

void FaceRenderer::updateBob(uint32_t nowMs) {
  state_.bobY = static_cast<float>((nowMs / 280U) % 3U) - 1.0f;
}

void FaceRenderer::drawEye(int16_t centerX, int16_t centerY, int16_t width, int16_t height, bool leftEye) {
  const int16_t eyeHeight = static_cast<int16_t>(height * state_.eyeOpen);
  tft_.fillRoundRect(centerX - width / 2, centerY - eyeHeight / 2, width, max<int16_t>(eyeHeight, 8), 18, kEyeWhite);

  const int16_t pupilX = centerX + static_cast<int16_t>(state_.gazeX);
  const int16_t pupilY = centerY + static_cast<int16_t>(state_.gazeY);
  const int16_t pupilR = state_.expression == FaceExpression::Curious ? 10 : 8;
  tft_.fillCircle(pupilX, pupilY, pupilR, kBg);

  const int16_t highlightX = pupilX + (leftEye ? 5 : -5);
  tft_.fillCircle(highlightX, pupilY - 5, 3, state_.accentColor);
}

void FaceRenderer::drawMouth() {
  const int16_t mouthY = 160 + static_cast<int16_t>(state_.bobY);
  tft_.fillRect(92, 150, 56, 24, kBg);

  switch (state_.expression) {
    case FaceExpression::Happy:
      tft_.drawRoundRect(96, mouthY, 48, 10, 5, ST77XX_WHITE);
      break;
    case FaceExpression::Curious:
      tft_.drawCircle(120, mouthY + 5, 5, ST77XX_WHITE);
      break;
    case FaceExpression::Sleepy:
      tft_.drawFastHLine(102, mouthY + 4, 36, ST77XX_WHITE);
      break;
    case FaceExpression::AngryPout:
      tft_.drawLine(100, mouthY + 8, 120, mouthY + 12, ST77XX_WHITE);
      tft_.drawLine(120, mouthY + 12, 140, mouthY + 8, ST77XX_WHITE);
      break;
    case FaceExpression::Neutral:
    default:
      tft_.drawFastHLine(102, mouthY + 6, 36, ST77XX_WHITE);
      break;
  }
}
```

- [ ] **Step 3: Verify face scope**

Confirm the file includes:
- black background
- large white eyes
- accent highlights
- 5 expressions
- blink, gaze drift, and micro bob

Expected: first version matches the approved visual scope without extra features.

- [ ] **Step 4: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/face_renderer.h D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/face_renderer.cpp
git commit -m "feat: add idle face renderer"
```

### Task 5: Extract the Usage Card Renderer

**Files:**
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_card_renderer.h`
- Create: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\usage_card_renderer.cpp`

- [ ] **Step 1: Create the card renderer header**

```cpp
#pragma once

#include <Adafruit_ST7789.h>

#include "display_types.h"

class UsageCardRenderer {
 public:
  explicit UsageCardRenderer(Adafruit_ST7789& tft);

  void renderFrame(const UsageData& data, bool fullRedraw);

 private:
  void clearRect(int x, int y, int w, int h);
  void drawStaticCard(int x, int y, int w, int h);
  void drawDynamicCard(const UsageData& data, int x, int y, int w, int h);

  Adafruit_ST7789& tft_;
};
```

- [ ] **Step 2: Move the current card drawing logic into the source file**

```cpp
#include "usage_card_renderer.h"

UsageCardRenderer::UsageCardRenderer(Adafruit_ST7789& tft) : tft_(tft) {}

void UsageCardRenderer::clearRect(int x, int y, int w, int h) {
  tft_.fillRect(x, y, w, h, ST77XX_BLACK);
}

void UsageCardRenderer::drawStaticCard(int x, int y, int w, int h) {
  const uint16_t frame = ST77XX_WHITE;
  const uint16_t accent = ST77XX_CYAN;
  const uint16_t muted = ST77XX_BLUE;

  tft_.setTextColor(accent, ST77XX_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(x + 14, y + 12);
  tft_.print("Codex");

  tft_.setTextColor(muted, ST77XX_BLACK);
  tft_.setTextSize(1);
  tft_.setCursor(x + 16, y + 32);
  tft_.print("remaining");

  tft_.setTextColor(frame, ST77XX_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(x + 16, y + 62);
  tft_.print("5h");

  tft_.drawRoundRect(x + 16, y + 82, w - 32, 32, 10, muted);

  tft_.setTextColor(frame, ST77XX_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(x + 16, y + 134);
  tft_.print("7d");

  tft_.drawRoundRect(x + 16, y + 154, w - 32, 22, 9, muted);

  tft_.setTextColor(muted, ST77XX_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(x + 16, y + 192);
  tft_.print("Reset");
}

void UsageCardRenderer::drawDynamicCard(const UsageData& data, int x, int y, int w, int h) {
  const uint16_t frame = ST77XX_WHITE;
  const uint16_t accent = ST77XX_CYAN;
  const uint16_t weekAccent = ST77XX_MAGENTA;

  clearRect(x + w - 116, y + 8, 104, 44);
  tft_.setTextColor(frame, ST77XX_BLACK);
  tft_.setTextSize(5);
  tft_.setCursor(x + w - 110, y + 10);
  tft_.print(String(data.windowPct) + "%");

  clearRect(x + 19, y + 85, w - 38, 26);
  tft_.fillRoundRect(x + 19, y + 85, (w - 38) * data.windowPct / 100, 26, 8, accent);

  clearRect(x + 19, y + 157, w - 38, 16);
  tft_.fillRoundRect(x + 19, y + 157, (w - 38) * data.weekPct / 100, 16, 7, weekAccent);
  clearRect(x + w - 76, y + 131, 62, 18);
  tft_.setTextColor(frame, ST77XX_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(x + w - 72, y + 134);
  tft_.print(String(data.weekPct) + "%");

  clearRect(x + 92, y + 192, 54, 16);
  tft_.setTextColor(frame, ST77XX_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(x + 92, y + 192);
  tft_.print(data.resetText);

  clearRect(x + 152, y + 192, 52, 16);
  tft_.setTextColor(ST77XX_BLUE, ST77XX_BLACK);
  tft_.setTextSize(1);
  tft_.setCursor(x + 154, y + 196);
  tft_.print(data.syncText);
}

void UsageCardRenderer::renderFrame(const UsageData& data, bool fullRedraw) {
  if (fullRedraw) {
    tft_.fillScreen(ST77XX_BLACK);
    drawStaticCard(16, 2, 208, 220);
  }
  drawDynamicCard(data, 16, 2, 208, 220);
}
```

- [ ] **Step 3: Check parity with current UI**

Compare the extracted card renderer against the old `main.cpp` card code and confirm:
- same geometry
- same colors
- same text layout

Expected: no visible usage-card regression before transitions are added.

- [ ] **Step 4: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/usage_card_renderer.h D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/usage_card_renderer.cpp
git commit -m "refactor: extract usage card renderer"
```

### Task 6: Wire the New Modules into `main.cpp`

**Files:**
- Modify: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\main.cpp`

- [ ] **Step 1: Replace the inline display state and helpers with module includes**

```cpp
#include "display_types.h"
#include "face_renderer.h"
#include "screen_mode_controller.h"
#include "usage_card_renderer.h"
#include "usage_state_tracker.h"
```

Add these globals near the existing `tft` object:

```cpp
UsageStateTracker usageTracker;
ScreenModeController modeController;
FaceRenderer faceRenderer(tft);
UsageCardRenderer usageCardRenderer(tft);
```

- [ ] **Step 2: Replace the old `CodexData` usage storage with `UsageData`**

```cpp
String serialBuffer;
unsigned long lastFetchAt = 0;
unsigned long lastWifiAttemptAt = 0;
int wifiIndex = 0;
bool wifiWasConnected = false;
```

Remove the old inline `CodexData codex;` struct and any now-redundant card drawing helpers moved into `usage_card_renderer.cpp`.

- [ ] **Step 3: Update JSON parsing to feed the tracker**

Replace the old payload application function with:

```cpp
bool applyJsonPayload(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }

  UsageData next = usageTracker.data();
  next.windowPct = doc["window_pct"] | next.windowPct;
  next.weekPct = doc["week_pct"] | next.weekPct;
  next.resetMin = doc["reset_min"] | next.resetMin;
  next.resetText = String(static_cast<const char*>(doc["reset_text"] | next.resetText.c_str()));

  const bool changed = usageTracker.applyIncomingData(next, millis());
  if (changed) {
    modeController.setMode(ScreenMode::UsageCard, millis());
  }
  return true;
}
```

- [ ] **Step 4: Update sync text handling to preserve non-trigger fields**

```cpp
void setSyncText(const String& text) {
  UsageData next = usageTracker.data();
  if (next.syncText == text) {
    return;
  }
  next.syncText = text;
  usageTracker.applyIncomingData(next, millis());
}
```

- [ ] **Step 5: Add a single display update function for mode and transitions**

```cpp
void updateDisplay() {
  const uint32_t nowMs = millis();

  modeController.setMode(
      usageTracker.isUsageActive(nowMs) ? ScreenMode::UsageCard : ScreenMode::Face,
      nowMs);
  modeController.update(nowMs);
  faceRenderer.update(nowMs);

  const bool fullRedraw = modeController.needsFullRedraw();
  const ScreenMode visibleMode =
      modeController.transition().active ? modeController.targetMode() : modeController.currentMode();

  if (visibleMode == ScreenMode::UsageCard) {
    usageCardRenderer.renderFrame(usageTracker.data(), fullRedraw);
  } else {
    faceRenderer.render(fullRedraw);
  }

  modeController.consumeFullRedrawFlag();
}
```

- [ ] **Step 6: Call the new display flow from setup and loop**

In `setup()` add:

```cpp
faceRenderer.begin();
updateDisplay();
```

In `loop()` replace direct card rendering calls with:

```cpp
updateDisplay();
delay(50);
```

- [ ] **Step 7: Build the firmware**

Run: `pio run`

Expected: `SUCCESS` with all new `.cpp` files compiled for `esp32-c3-supermini`.

- [ ] **Step 8: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/main.cpp
git commit -m "feat: wire face mode and usage mode controller"
```

### Task 7: Add Lightweight Visual Transitions

**Files:**
- Modify: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\main.cpp`
- Modify: `D:\工作文件\工作文件\14.ai项目\esp32\cc-island-c3\src\screen_mode_controller.cpp`

- [ ] **Step 1: Update the controller so transition progress reaches 255 only at completion**

Adjust `computeProgress` to return an eased stepped value:

```cpp
uint8_t computeProgress(uint32_t nowMs, const TransitionState& state) {
  if (!state.active || state.durationMs == 0) {
    return 255;
  }

  const uint32_t elapsed = nowMs - state.startedAtMs;
  if (elapsed >= state.durationMs) {
    return 255;
  }

  const uint32_t linear = (elapsed * 255U) / state.durationMs;
  return static_cast<uint8_t>((linear / 32U) * 32U);
}
```

- [ ] **Step 2: Add a stepped wipe renderer in `main.cpp`**

Inside `updateDisplay()`, replace the simple target-mode draw with:

```cpp
const TransitionState& transition = modeController.transition();
if (transition.active) {
  const int wipeHeight = (240 * transition.progress) / 255;

  if (transition.direction == TransitionDirection::FaceToCard) {
    faceRenderer.render(fullRedraw);
    usageCardRenderer.renderFrame(usageTracker.data(), false);
    tft.fillRect(0, wipeHeight, 240, 240 - wipeHeight, ST77XX_BLACK);
    faceRenderer.render(false);
  } else {
    usageCardRenderer.renderFrame(usageTracker.data(), fullRedraw);
    faceRenderer.render(false);
    tft.fillRect(0, wipeHeight, 240, 240 - wipeHeight, ST77XX_BLACK);
    usageCardRenderer.renderFrame(usageTracker.data(), false);
  }
} else if (visibleMode == ScreenMode::UsageCard) {
  usageCardRenderer.renderFrame(usageTracker.data(), fullRedraw);
} else {
  faceRenderer.render(fullRedraw);
}
```

- [ ] **Step 3: Rebuild after transition changes**

Run: `pio run`

Expected: `SUCCESS` and no duplicate symbol or missing include errors.

- [ ] **Step 4: Commit**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/main.cpp D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src/screen_mode_controller.cpp
git commit -m "feat: add lightweight screen transitions"
```

### Task 8: Manual Validation with Existing Inputs

**Files:**
- No source changes required

- [ ] **Step 1: Flash the firmware**

Run: `pio run -t upload`

Expected: upload completes successfully to `COM3`.

- [ ] **Step 2: Open the serial monitor**

Run: `pio device monitor -b 115200`

Expected: boot log includes `codex-card: ready`.

- [ ] **Step 3: Verify idle behavior on boot**

Observe:
- display boots into face mode
- eyes blink within a few seconds
- gaze shifts subtly
- no card appears until real usage data changes

Expected: face mode is stable and animated.

- [ ] **Step 4: Send one payload with changed values**

Run from `D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/pc_bridge`:

```bash
python send_serial.py --port COM3 --json sample_payload.json
```

Expected:
- display transitions to usage card
- card shows the payload values
- 3-minute usage window begins

- [ ] **Step 5: Send the same payload again**

Run:

```bash
python send_serial.py --port COM3 --json sample_payload.json
```

Expected:
- card remains visible if still inside the active window
- active window is not renewed by identical values
- no abrupt transition retriggers

- [ ] **Step 6: Send a modified payload**

Edit `sample_payload.json` temporarily so one of these fields changes:
- `window_pct`
- `week_pct`
- `reset_text`

Then run:

```bash
python send_serial.py --port COM3 --json sample_payload.json
```

Expected:
- card values update
- the 3-minute active window renews from this new change

- [ ] **Step 7: Wait for idle timeout**

Wait just over 3 minutes without sending any changed payload.

Expected:
- usage card transitions back to face mode
- face resumes idle animation cleanly

- [ ] **Step 8: Commit if any validation-only tweaks were needed**

```bash
git add D:/工作文件/工作文件/14.ai项目/esp32/cc-island-c3/src
git commit -m "chore: tune face display behavior after manual validation"
```

## Self-Review

### Spec Coverage

- Dual-mode display: Tasks 3, 6, 7
- StackChan-inspired idle face: Task 4
- Usage shown only on meaningful change: Tasks 2, 6
- 3-minute renewal window: Task 2
- Smooth transition back and forth: Tasks 3, 7
- Preserve current usage card concept: Task 5
- Manual validation with real payloads: Task 8

### Placeholder Scan

No `TODO`, `TBD`, or unresolved file references remain in the plan. Every file path, class name, and command is explicit.

### Type Consistency

Shared names stay consistent across tasks:
- `UsageData`
- `UsageStateTracker`
- `ScreenModeController`
- `FaceRenderer`
- `UsageCardRenderer`
- `TransitionState`

