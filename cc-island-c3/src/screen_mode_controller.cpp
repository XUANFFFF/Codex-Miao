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

  const uint32_t linear = (elapsed * 255U) / state.durationMs;
  const uint32_t eased = (linear * linear * (765U - 2U * linear)) / 65025U;
  return static_cast<uint8_t>(eased);
}

TransitionDirection transitionDirectionFor(ScreenMode nextMode) {
  return nextMode == ScreenMode::UsageCard ? TransitionDirection::FaceToCard
                                           : TransitionDirection::CardToFace;
}

}  // namespace

ScreenModeController::ScreenModeController() = default;

void ScreenModeController::setMode(ScreenMode nextMode, uint32_t nowMs) {
  if (nextMode == targetMode_) {
    return;
  }

  if (!transition_.active && nextMode == currentMode_) {
    return;
  }

  targetMode_ = nextMode;
  needsFullRedraw_ = true;

  if (targetMode_ == currentMode_) {
    transition_ = TransitionState{};
    return;
  }

  transition_ = TransitionState{};
  transition_.active = true;
  transition_.startedAtMs = nowMs;
  transition_.direction = transitionDirectionFor(targetMode_);
}

void ScreenModeController::update(uint32_t nowMs) {
  if (!transition_.active) {
    return;
  }

  transition_.progress = computeProgress(nowMs, transition_);
  if (transition_.progress < 255) {
    return;
  }

  currentMode_ = targetMode_;
  transition_ = TransitionState{};
  needsFullRedraw_ = true;
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
