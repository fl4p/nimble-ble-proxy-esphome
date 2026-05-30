// Optional WiFi NAT router (gated on CONFIG_NBP_NAT_ROUTER). Brings up a
// SoftAP in APSTA mode alongside the already-connected STA and NATs AP
// client traffic out through the STA uplink. Inbound port-forwarding
// rules are runtime-configurable via the dashboard (/nat, /portmap) and
// persisted in NVS.
//
// Declarations exist only when CONFIG_NBP_NAT_ROUTER=y; callers guard
// their calls behind the same #if (see main.cpp). sdkconfig.h must be
// pulled in here so both the .cpp and main see the gate macro — IDF v5.5
// does not auto-inject it.

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_NAT_ROUTER

namespace nat_router {

// Create the SoftAP, switch to APSTA, enable NAPT on the AP netif, and
// apply persisted port mappings. Call AFTER wifi_sta::start_and_wait_for_ip()
// so the STA is associated and has an IP + DNS to mirror to AP clients.
void start();

// Register /nat (GET status / POST AP creds) and /portmap (POST add or
// delete) on the shared dashboard httpd. Pass the httpd_handle_t from
// ota::handle(); a null handle disables the endpoints.
void register_endpoints(void *httpd_handle);

// Drop / restore the SoftAP around an OTA download so the radio isn't split
// between APSTA forwarding and the firmware upload (see ota::set_quiesce_hooks).
// pause_for_ota() tears the AP down (the STA link stays up, so the OTA stream
// and dashboard survive); resume_after_ota() brings it back only if it had
// been up — invoked solely on OTA failure, since success reboots. Wired by main.
void pause_for_ota();
void resume_after_ota();

}  // namespace nat_router

#endif  // CONFIG_NBP_NAT_ROUTER
