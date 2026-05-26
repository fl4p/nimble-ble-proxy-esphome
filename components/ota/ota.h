// Tiny HTTP OTA server. POST a raw .bin to /update to flash the
// inactive OTA slot and reboot. No auth — assumes a trusted LAN.
//
// Usage:
//   curl --data-binary @nimble_ble_proxy.bin http://nimble-proxy.local/update

#pragma once

#include "esp_http_server.h"

namespace ota {

// Start the HTTP server. Call once after WiFi has an IP.
void start();

// Shared httpd handle (nullptr until start() succeeds). Other
// components piggyback URIs on this so we only open one listener —
// LWIP_MAX_SOCKETS is tight on ESP32.
httpd_handle_t handle();

}  // namespace ota
