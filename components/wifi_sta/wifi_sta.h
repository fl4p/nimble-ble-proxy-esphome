// Minimal WiFi STA bring-up. Reads SSID/password from
// include/wifi_creds.h (gitignored — copy from wifi_creds.h.example).
// Blocks until an IP is assigned, then returns. Auto-reconnects in
// the background if the link drops later.

#pragma once

#include <cstddef>

namespace wifi_sta {

void start_and_wait_for_ip();

// True while the STA is associated and holds an IP. Set on
// IP_EVENT_STA_GOT_IP, cleared on WIFI_EVENT_STA_DISCONNECTED. Cheap,
// lock-free — used by the BLE auto-off supervisor to decide whether the
// dashboard is reachable over WiFi (so the BLE peripheral can sleep).
bool is_connected();

// Disconnect bookkeeping, appended to /stats.json (via a provider wired in
// main): ,"sta_disc":N,"sta_disc_reason":R,"sta_disc_rssi":dBm,
// "sta_disc_down_ms":ms,"ap_disc":N,"ap_disc_reason":R,"ap_disc_mac":"…".
// sta_disc counts upstream link losses (not failed retries); the reason,
// RSSI and outage length are the last one's. ap_disc counts SoftAP client
// departures. Returns what snprintf returns.
int append_disc_stats_json(char *buf, size_t cap);

// GET /wifidisc: the last 8 disconnect events (STA and SoftAP), newest
// first, with reason names, uptime stamps and STA outage lengths.
void register_endpoints(void *httpd_handle);

}  // namespace wifi_sta
