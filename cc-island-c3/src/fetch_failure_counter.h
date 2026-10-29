#pragma once

#include <stdint.h>

class FetchFailureCounter {
 public:
  void reset() { count_ = 0; }

  void noteEndpointSeen(bool endpointChanged) {
    if (endpointChanged) {
      reset();
    }
  }

  bool noteFailure() {
    if (count_ < kMaxConsecutiveFailures) {
      ++count_;
    }
    return count_ >= kMaxConsecutiveFailures;
  }

  uint32_t count() const { return count_; }

 private:
  static constexpr uint32_t kMaxConsecutiveFailures = 3;
  uint32_t count_ = 0;
};
