// Tiny HTTP OTA server. POST a raw .bin to /update to flash the
// inactive OTA slot and reboot. No auth — assumes a trusted LAN.
//
// Usage:
//   curl --data-binary @nimble_ble_proxy.bin http://nimble-proxy.local/update

#pragma once

namespace ota {

// Start the HTTP server. Call once after WiFi has an IP.
void start();

}  // namespace ota
