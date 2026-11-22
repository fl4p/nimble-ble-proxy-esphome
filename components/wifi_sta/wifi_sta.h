// Minimal WiFi STA bring-up. Reads SSID/password from
// include/wifi_creds.h (gitignored — copy from wifi_creds.h.example).
// Blocks until an IP is assigned, then returns. Auto-reconnects in
// the background if the link drops later.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

#if CONFIG_NBP_IMPROV
#include "esp_wifi_types.h"
#endif

namespace wifi_sta {

#if CONFIG_NBP_IMPROV
// A second provisioning transport (Improv over BLE) next to the SoftAP form.
// It exists only inside the provisioning window: the hooks fire from the
// provisioning loop, which stays the only code that touches the radio.
// request_join()/request_scan() only queue work for that loop.
struct ProvisioningHooks {
  void (*entered)();  // provisioning mode is up (SoftAP running)
  // Outcome of request_join(). On success `url` is "http://<ip>/" and the
  // device reboots into normal mode about 2 s later.
  void (*join_done)(bool ok, const char *url);
  // Outcome of request_scan(); records strongest first, valid during the call.
  void (*scan_done)(bool ok, const wifi_ap_record_t *aps, uint16_t n);
};
void set_provisioning_hooks(const ProvisioningHooks *hooks);

enum class Request : uint8_t {
  Queued,
  Invalid,  // SSID/password can never work (length, NUL byte)
  Busy,     // another join or scan is in flight
  Closed,   // not in the provisioning window, or it is ending
};
Request request_join(const char *ssid, size_t ssid_len, const char *psk,
                     size_t psk_len);
Request request_scan();
#endif

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
