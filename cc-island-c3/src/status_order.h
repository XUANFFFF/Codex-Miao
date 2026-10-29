#pragma once

#include <stdint.h>
#include <string.h>

// A stream belongs to one Bridge process. Retired streams cannot take over
// again when an old HTTP/USB response arrives after a Bridge restart.
class StatusOrder {
 public:
  bool accept(const char* stream, uint64_t revision) {
    if (stream == nullptr || stream[0] == '\0' || strlen(stream) > 32 || revision == 0) {
      return false;
    }
    if (strcmp(stream_, stream) == 0) {
      if (revision < revision_) return false;
      revision_ = revision;
      return true;
    }
    for (const auto& retired : retired_) {
      if (strcmp(retired, stream) == 0) return false;
    }
    if (stream_[0] != '\0') {
      strcpy(retired_[nextRetired_], stream_);
      nextRetired_ = (nextRetired_ + 1) % 4;
    }
    strcpy(stream_, stream);
    revision_ = revision;
    return true;
  }

  bool initialized() const { return stream_[0] != '\0'; }

 private:
  char stream_[33] = {};
  char retired_[4][33] = {};
  uint8_t nextRetired_ = 0;
  uint64_t revision_ = 0;
};
