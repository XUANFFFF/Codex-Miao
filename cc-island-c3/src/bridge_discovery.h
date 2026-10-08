#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

class BridgeDiscovery {
 public:
  void begin(uint16_t listenPort, const char* expectedService, uint32_t staleAfterMs);
  void reset();
  bool update(uint32_t nowMs);
  bool hasEndpoint() const;
  bool endpointUrl(char* buffer, size_t capacity, const char* pathSuffix) const;
  void noteFetchFailure(uint32_t nowMs);
  void noteFetchSuccess(uint32_t nowMs);

 private:
  static constexpr size_t kMaxServiceNameLength = 96;
  static constexpr uint32_t kMaxConsecutiveFetchFailures = 3;

  void clearRuntimeState();
  bool shouldExpire(uint32_t nowMs) const;

  WiFiUDP udp_;
  uint16_t listenPort_ = 0;
  uint16_t endpointPort_ = 0;
  uint32_t staleAfterMs_ = 0;
  uint32_t lastBeaconAtMs_ = 0;
  bool hasLastBeaconAtMs_ = false;
  uint32_t lastFetchFailureAtMs_ = 0;
  uint32_t lastFetchSuccessAtMs_ = 0;
  uint32_t failureCount_ = 0;
  IPAddress endpointIp_;
  bool listening_ = false;
  bool endpointValid_ = false;
  char expectedService_[kMaxServiceNameLength + 1] = {0};
};
