#pragma once

// Copy this file to wifi_secrets.h and fill in local network credentials and token.
// wifi_secrets.h is excluded from Git. Bridge discovery verifies the token signature.
static constexpr WifiNetwork kWifiNetworks[] = {
    {"YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD"},
};
// Optional host filter for signed discovery; empty accepts any valid signed bridge beacon.
static constexpr char kBridgePreferredHost[] = "";
// Generate a random 64-character hexadecimal token and use the same value on the PC bridge.
static constexpr char kBridgeAuthToken[] = "REPLACE_WITH_64_HEX_CHAR_TOKEN";
