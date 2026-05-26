// Minimal WiFi STA bring-up. Reads SSID/password from
// include/wifi_creds.h (gitignored — copy from wifi_creds.h.example).
// Blocks until an IP is assigned, then returns. Auto-reconnects in
// the background if the link drops later.

#pragma once

namespace wifi_sta {

void start_and_wait_for_ip();

}  // namespace wifi_sta
