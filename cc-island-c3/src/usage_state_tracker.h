#pragma once

#include <stdint.h>

#include "display_types.h"

class UsageStateTracker {
 public:
  static constexpr uint32_t kActiveWindowMs = 180000;

  UsageStateTracker();

  // Returns true only when the payload counts as meaningful usage activity.
  bool applyIncomingData(const UsageData& incoming, uint32_t nowMs);
  const UsageData& data() const;
  bool isUsageActive(uint32_t nowMs) const;
  uint32_t activeUntilMs() const;

 private:
  UsageData current_;
  uint32_t activeUntilMs_ = 0;
  bool initialized_ = false;
};
