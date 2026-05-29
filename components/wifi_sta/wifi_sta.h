// Minimal WiFi STA bring-up. Reads SSID/password from
// include/wifi_creds.h (gitignored — copy from wifi_creds.h.example).
// Blocks until an IP is assigned, then returns. Auto-reconnects in
// the background if the link drops later.

#pragma once

namespace wifi_sta {

void start_and_wait_for_ip();

// True while the STA is associated and holds an IP. Set on
// IP_EVENT_STA_GOT_IP, cleared on WIFI_EVENT_STA_DISCONNECTED. Cheap,
// lock-free — used by the BLE auto-off supervisor to decide whether the
// dashboard is reachable over WiFi (so the BLE peripheral can sleep).
bool is_connected();

}  // namespace wifi_sta
