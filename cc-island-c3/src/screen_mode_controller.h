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
