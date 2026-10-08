#include "usage_state_tracker.h"

#include <string.h>

namespace {
bool textEquals(const char* left, const char* right, size_t capacity) {
  return strncmp(left, right, capacity) == 0;
}

void copyText(char* dest, const char* src, size_t capacity) {
  if (capacity == 0) {
    return;
  }

  strncpy(dest, src, capacity);
  dest[capacity - 1] = '\0';
}
}  // namespace

UsageStateTracker::UsageStateTracker() = default;

bool UsageStateTracker::applyIncomingData(const UsageData& incoming, uint32_t nowMs) {
  const bool hasMeaningfulActivity =
      initialized_ &&
      (incoming.windowPct < current_.windowPct || incoming.weekPct < current_.weekPct);

  current_.windowPct = incoming.windowPct;
  current_.weekPct = incoming.weekPct;
  current_.resetMin = incoming.resetMin;
  copyText(current_.resetText, incoming.resetText, kResetTextCapacity);
  copyText(current_.syncText, incoming.syncText, kSyncTextCapacity);
  initialized_ = true;

  if (hasMeaningfulActivity) {
    activeUntilMs_ = nowMs + kActiveWindowMs;
  }

  return hasMeaningfulActivity;
}

const UsageData& UsageStateTracker::data() const {
  return current_;
}

void UsageStateTracker::forceIdle() {
  activeUntilMs_ = 0;
  initialized_ = false;
}

bool UsageStateTracker::hasData() const {
  return initialized_;
}

bool UsageStateTracker::isUsageActive(uint32_t nowMs) const {
  return activeUntilMs_ != 0 && static_cast<int32_t>(activeUntilMs_ - nowMs) > 0;
}

uint32_t UsageStateTracker::activeUntilMs() const {
  return activeUntilMs_;
}
