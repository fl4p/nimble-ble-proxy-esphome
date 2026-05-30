// Tiny HTTP OTA server. POST a raw .bin to /update to flash the
// inactive OTA slot and reboot. No auth — assumes a trusted LAN.
//
// Usage:
//   curl --data-binary @nimble_ble_proxy.bin http://nimble-proxy.local/update
//
// When NBP_OTA=n, the /update handler is not registered (and
// esp_ota_ops is not linked), but start() still brings up the shared
// httpd listener that the dashboard / api_server / clone piggyback on.

#pragma once

#include "esp_http_server.h"

namespace ota {

// Start the HTTP server. Call once after WiFi has an IP.
void start();

// Optional radio-quiesce hooks for the duration of an OTA download. On this
// single-radio, no-PSRAM S3 a firmware upload competes for airtime with the
// BLE scan and the NAT/SoftAP forward path; under load the upload is starved
// to a crawl (the coex/SA-Query death-spiral) and the OTA times out. `begin`
// is invoked once the upload starts to free the radio (drop the SoftAP, pause
// the BLE scan); `end` is invoked only if the OTA fails, to restore them — on
// success the device reboots, which restores everything anyway. Both may be
// null (no quiescing). Wired by main to avoid ota depending on nat_router /
// ble_backend; mirrors the api_server provider-callback pattern.
using QuiesceFn = void (*)();
void set_quiesce_hooks(QuiesceFn begin, QuiesceFn end);

// Shared httpd handle (nullptr until start() succeeds). Other
// components piggyback URIs on this so we only open one listener —
// LWIP_MAX_SOCKETS is tight on ESP32.
httpd_handle_t handle();

}  // namespace ota
