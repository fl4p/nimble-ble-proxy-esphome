// Project-wide configuration constants. Edit these to rebrand the device
// or tune the BLE proxy. Nothing here is runtime-configurable in v1.

#pragma once

#include <cstddef>
#include <cstdint>

namespace proxy {

inline constexpr const char *VERSION = "0.1.0";

// Identity advertised to Home Assistant.
inline constexpr const char *HOSTNAME = "nimble-proxy";
inline constexpr const char *FRIENDLY_NAME = "NimBLE Proxy";
inline constexpr const char *MODEL = "esp32-s3-devkitc";
inline constexpr const char *MANUFACTURER = "Custom";

// Reported as DeviceInfoResponse.esphome_version. HA only sanity-checks this,
// but it should look like a current release so the integration doesn't warn.
inline constexpr const char *FAKE_ESPHOME_VERSION = "2026.5.0";

// aioesphomeapi protocol version we claim to speak. 1.14 is current as of
// ESPHome 2026.x; bumping just adds optional fields, never breaks framing.
inline constexpr uint32_t API_VERSION_MAJOR = 1;
inline constexpr uint32_t API_VERSION_MINOR = 14;

// Plaintext API port — fixed by ESPHome convention.
inline constexpr uint16_t API_PORT = 6053;

// Bluetooth proxy feature flags advertised to HA.
//   bit 0 = PASSIVE_SCAN
//   bit 1 = ACTIVE_CONNECTIONS
//   bit 2 = REMOTE_CACHING  (REQUIRED by modern aioesphomeapi for any
//                            GATT connection through the proxy — we always
//                            do a fresh discovery so we ignore cache hints,
//                            but HA needs to see this bit to proceed)
//   bit 5 = RAW_ADVERTISEMENTS
// See bluetooth_proxy.h:42-50 in esphome for the full enum.
inline constexpr uint32_t BT_PROXY_FEATURE_FLAGS =
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5);

// BLE proxy tuning.
// 9 matches ESPHome's BT proxy cap and what NimBLE can comfortably handle
// on ESP32-S3. Keep in sync with CONFIG_BT_NIMBLE_MAX_CONNECTIONS in
// sdkconfig.defaults AND proxyapi.BluetoothConnectionsFreeResponse.allocated
// max_count in components/api_proto/api_subset.options.
inline constexpr uint8_t MAX_CONNECTIONS = 9;
inline constexpr uint16_t SCAN_INTERVAL_MS = 30;
inline constexpr uint16_t SCAN_WINDOW_MS = 30;
inline constexpr uint8_t ADV_BATCH_SIZE = 16;
inline constexpr uint32_t ADV_FLUSH_INTERVAL_MS = 100;

// GATT discovery batching threshold — ESPHome chunks at ~1360 B per
// BluetoothGATTGetServicesResponse to fit comfortably in a typical MTU stream.
inline constexpr size_t GATT_DISCOVERY_CHUNK_BYTES = 1360;

// Per-connection timeouts.
inline constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
inline constexpr uint32_t DISCONNECT_TIMEOUT_MS = 10000;

// API frame limits — matches ESPHome's MAX_MESSAGE_SIZE for plaintext.
inline constexpr size_t MAX_MESSAGE_SIZE = 2048;

// Concurrent API clients. ESPHome's default is 4; we match that so HA
// can stay connected while a CLI client (esphome / aioesphomeapi) also
// inspects the device. Each slot costs ~4 KiB stack + one socket.
inline constexpr uint8_t MAX_API_CLIENTS = 4;

}  // namespace proxy
