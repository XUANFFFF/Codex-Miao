#include "bridge_discovery.h"

#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>

#include "wifi_config.h"

namespace {
constexpr size_t kBeaconBufferSize = 384;
constexpr size_t kDiscardBufferSize = 64;

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

bool verifyBeaconSignature(const char* service, long version, const char* host, long port,
                           const char* signature) {
  if (service == nullptr || host == nullptr || signature == nullptr) {
    return false;
  }

  char message[192];
  const int messageLength =
      snprintf(message, sizeof(message), "%s\n%ld\n%s\n%ld", service, version, host, port);
  if (messageLength <= 0 || static_cast<size_t>(messageLength) >= sizeof(message) ||
      strlen(signature) != 64) {
    return false;
  }

  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mdInfo == nullptr) {
    return false;
  }

  unsigned char expected[32];
  if (mbedtls_md_hmac(mdInfo, reinterpret_cast<const unsigned char*>(kBridgeAuthToken),
                      strlen(kBridgeAuthToken), reinterpret_cast<const unsigned char*>(message),
                      static_cast<size_t>(messageLength), expected) != 0) {
    return false;
  }

  unsigned char difference = 0;
  for (size_t index = 0; index < sizeof(expected); ++index) {
    const int high = hexValue(signature[index * 2]);
    const int low = hexValue(signature[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    const unsigned char supplied = static_cast<unsigned char>((high << 4) | low);
    difference |= expected[index] ^ supplied;
  }
  return difference == 0;
}

void copyServiceName(char* dest, size_t capacity, const char* src) {
  if (dest == nullptr || capacity == 0) {
    return;
  }

  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }

  strncpy(dest, src, capacity);
  dest[capacity - 1] = '\0';
}

bool serviceMatches(const char* expected, const char* actual) {
  if (expected == nullptr || expected[0] == '\0') {
    return false;
  }

  if (actual == nullptr) {
    return false;
  }

  return strcmp(expected, actual) == 0;
}

bool parseIpv4Address(const char* text, IPAddress* out) {
  if (text == nullptr || out == nullptr) {
    return false;
  }

  IPAddress parsed;
  if (!parsed.fromString(text)) {
    return false;
  }

  if (parsed[0] == 127 || (parsed[0] == 169 && parsed[1] == 254)) {
    return false;
  }

  *out = parsed;
  return true;
}
}  // namespace

void BridgeDiscovery::clearRuntimeState() {
  endpointIp_ = IPAddress();
  endpointPort_ = 0;
  lastBeaconAtMs_ = 0;
  hasLastBeaconAtMs_ = false;
  lastFetchFailureAtMs_ = 0;
  lastFetchSuccessAtMs_ = 0;
  fetchFailures_.reset();
  endpointValid_ = false;
}

bool BridgeDiscovery::shouldExpire(uint32_t nowMs) const {
  if (!endpointValid_ || staleAfterMs_ == 0 || !hasLastBeaconAtMs_) {
    return false;
  }

  return static_cast<uint32_t>(nowMs - lastBeaconAtMs_) > staleAfterMs_;
}

void BridgeDiscovery::begin(uint16_t listenPort, const char* expectedService,
                            uint32_t staleAfterMs) {
  udp_.stop();
  clearRuntimeState();

  listenPort_ = listenPort;
  staleAfterMs_ = staleAfterMs;
  copyServiceName(expectedService_, sizeof(expectedService_), expectedService);

  listening_ = udp_.begin(listenPort_);
  Serial.printf("discovery: listen port=%u status=%s\n", listenPort_,
                listening_ ? "ok" : "failed");
}

void BridgeDiscovery::reset() {
  clearRuntimeState();
}

bool BridgeDiscovery::update(uint32_t nowMs) {
  if (!listening_ && listenPort_ != 0) {
    listening_ = udp_.begin(listenPort_);
    if (listening_) {
      Serial.printf("discovery: retry listen port=%u status=ok\n", listenPort_);
    }
  }

  if (shouldExpire(nowMs)) {
    Serial.println("discovery: endpoint expired");
    clearRuntimeState();
  }

  if (listening_) {
    int packetSize = 0;
    while ((packetSize = udp_.parsePacket()) > 0) {
      if (packetSize >= static_cast<int>(kBeaconBufferSize)) {
        char discard[kDiscardBufferSize];
        int remaining = packetSize;
        while (remaining > 0) {
          const size_t chunkSize = static_cast<size_t>(
              remaining > static_cast<int>(kDiscardBufferSize) ? kDiscardBufferSize : remaining);
          const int bytesRead = udp_.read(reinterpret_cast<uint8_t*>(discard), chunkSize);
          if (bytesRead <= 0) {
            break;
          }
          remaining -= bytesRead;
        }
        continue;
      }

      char packet[kBeaconBufferSize];
      const int bytesRead =
          udp_.read(reinterpret_cast<uint8_t*>(packet), static_cast<size_t>(packetSize));
      if (bytesRead != packetSize || bytesRead <= 0) {
        continue;
      }
      packet[bytesRead] = '\0';

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, packet);
      if (error) {
        continue;
      }

      const char* service = doc["service"] | nullptr;
      if (!serviceMatches(expectedService_, service)) {
        continue;
      }

      const long versionValue = doc["version"] | -1L;
      const long portValue = doc["port"] | -1L;
      if (versionValue != 1L || portValue <= 0 || portValue > 65535L) {
        continue;
      }

      const char* advertisedHost = doc["host"] | nullptr;
      const char* signature = doc["signature"] | nullptr;
      IPAddress nextIp;
      if (!parseIpv4Address(advertisedHost, &nextIp) ||
          nextIp != udp_.remoteIP() ||
          !verifyBeaconSignature(service, versionValue, advertisedHost, portValue, signature)) {
        continue;
      }
      if (kBridgePreferredHost[0] != '\0' && strcmp(kBridgePreferredHost, advertisedHost) != 0) {
        continue;
      }
      const uint16_t nextPort = static_cast<uint16_t>(portValue);
      const bool endpointChanged =
          !endpointValid_ || endpointIp_ != nextIp || endpointPort_ != nextPort;

      endpointIp_ = nextIp;
      endpointPort_ = nextPort;
      lastBeaconAtMs_ = nowMs;
      hasLastBeaconAtMs_ = true;
      fetchFailures_.noteEndpointSeen(endpointChanged);
      endpointValid_ = true;

      if (endpointChanged) {
        Serial.printf(
            "discovery: beacon host=%s remote=%u.%u.%u.%u endpoint=%u.%u.%u.%u:%u\n",
            advertisedHost != nullptr ? advertisedHost : "(none)", udp_.remoteIP()[0],
            udp_.remoteIP()[1], udp_.remoteIP()[2], udp_.remoteIP()[3], endpointIp_[0],
            endpointIp_[1], endpointIp_[2], endpointIp_[3], endpointPort_);
      }
    }
  }

  if (shouldExpire(nowMs)) {
    Serial.println("discovery: endpoint expired");
    clearRuntimeState();
  }

  return hasEndpoint();
}

bool BridgeDiscovery::hasEndpoint() const {
  return endpointValid_;
}

bool BridgeDiscovery::endpointUrl(char* buffer, size_t capacity, const char* pathSuffix) const {
  if (buffer == nullptr || capacity == 0 || !endpointValid_) {
    return false;
  }

  const char* suffix = (pathSuffix == nullptr) ? "" : pathSuffix;
  const bool needSlash = suffix[0] != '\0' && suffix[0] != '/';
  const int written =
      needSlash
          ? snprintf(buffer, capacity, "http://%u.%u.%u.%u:%u/%s", endpointIp_[0], endpointIp_[1],
                     endpointIp_[2], endpointIp_[3], endpointPort_, suffix)
          : snprintf(buffer, capacity, "http://%u.%u.%u.%u:%u%s", endpointIp_[0], endpointIp_[1],
                     endpointIp_[2], endpointIp_[3], endpointPort_, suffix);

  return written > 0 && static_cast<size_t>(written) < capacity;
}

void BridgeDiscovery::noteFetchFailure(uint32_t nowMs) {
  lastFetchFailureAtMs_ = nowMs;
  if (fetchFailures_.noteFailure()) {
    Serial.println("discovery: clearing endpoint after repeated fetch failures");
    clearRuntimeState();
  }
}

void BridgeDiscovery::noteFetchSuccess(uint32_t nowMs) {
  lastFetchSuccessAtMs_ = nowMs;
  fetchFailures_.reset();
}
