#pragma once

struct WifiNetwork {
  const char* ssid;
  const char* password;
};

static constexpr WifiNetwork kWifiNetworks[] = {
    {"Xiaomi_ZLF_5G", "2268888888888"},
    {"REPLACE_WITH_WIFI_NAME_2", "REPLACE_WITH_WIFI_PASSWORD_2"},
};

static constexpr int kWifiNetworkCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
static constexpr char kBridgeUrl[] = "http://192.168.31.178:8765/usage";
static constexpr unsigned long kFetchIntervalMs = 60000;
static constexpr unsigned long kWifiRetryMs = 10000;
