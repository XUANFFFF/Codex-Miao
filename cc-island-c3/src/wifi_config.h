#pragma once

struct WifiNetwork {
  const char* ssid;
  const char* password;
};

// Copy wifi_secrets.example.h to wifi_secrets.h and set local credentials.
#include "wifi_secrets.h"

static constexpr int kWifiNetworkCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
static constexpr uint16_t kBridgeDiscoveryPort = 8766;
static constexpr char kBridgeDiscoveryService[] = "codex-miao-bridge";
static constexpr char kBridgeChallengePath[] = "/challenge";
static constexpr char kBridgeUsagePath[] = "/usage";
static constexpr unsigned long kBridgeDiscoveryStaleMs = 15000;
static constexpr unsigned long kFetchIntervalMs = 3000;
static constexpr unsigned long kFetchRetryAfterFailureMs = 5000;
static constexpr unsigned long kWifiRetryMs = 10000;
