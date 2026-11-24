// OTA image verification on top of IDF app rollback
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
//
// An image installed by an OTA from a rollback-enabled app boots in otadata
// state NEW. A bootloader built with rollback turns that into PENDING_VERIFY
// and rolls back on the next reset unless the app confirms it; a bootloader
// built without rollback (every device flashed before this feature) leaves it
// NEW and boots it again after any reset. Both are handled here:
//
//  - The image marks itself valid once, within this boot, it has run
//    NBP_OTA_VERIFY_MIN_SECS AND the STA has an IP AND the uplink answered
//    (a liveness probe cycle passed, an upstream API session is live, the
//    gateway answered on :80 (connect or RST), or the gateway answered ARP)
//    AND its own httpd (:80) and api_server (:6053) listeners accept.
//  - Not met within NBP_OTA_VERIFY_DEADLINE_SECS: the image marks itself
//    invalid and reboots into the previous one. An esp_timer backstop
//    restarts the chip 60 s later if the worker that does this is stuck.
//  - Old bootloader only: the app counts its own unverified starts (RTC
//    memory) and rolls back at NBP_OTA_VERIFY_MAX_BOOTS. Crashes before
//    app_main are not counted there; a new bootloader catches any reset.
//
// The previous boot's outcome (rolled back from which image, why) is kept in
// NVS (namespace "otarb") and shown by GET /ota and GET /crash.

#pragma once

#include <cstddef>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_partition.h"

namespace ota_verify {

// Checks injected by main (ota keeps no dependency on wifi_sta, api_server or
// liveness_wdt). A null hook means "not applicable to this build" (e.g. no
// WiFi) and passes; until set_hooks() is called nothing counts as proof.
struct Hooks {
  bool (*sta_up)();
  bool (*uplink)(const char **via);  // *via: short name of the proof
  bool (*service)();
};

// Right after nvs_flash_init(). Classifies this boot (running slot, otadata
// state, bootloader capability), records a rollback the bootloader or the
// previous boot performed, and, if the running image is unverified, starts the
// verification worker and the deadline. May reboot (old-bootloader start
// counter). Cheap no-op when the image is already valid.
void early_init();

void set_hooks(const Hooks &h);

// The running image is not yet verified (NEW or PENDING_VERIFY and no verdict
// yet). /update refuses while this is true: it would overwrite the only
// rollback target.
bool pending();
// Seconds until the deadline (0 if not pending).
unsigned left_s();

// After esp_ota_set_boot_partition(target): remember which image is expected
// to boot next, so the next boot can tell a rollback from a normal boot.
void note_update(const esp_partition_t *target);

// Helpers main uses to build the hooks (WiFi builds).
// Gateway answered: TCP connect to gw:80 succeeded or was refused ("gateway"),
// else the gateway is in the ARP table ("arp"); no gateway configured
// ("no_gateway") also passes. False if the STA has no IP.
bool gateway_reachable(const char **via);
// A TCP connect to 127.0.0.1:port completes within 1 s.
bool loopback_listens(uint16_t port);

// JSON object describing the OTA state (GET /ota, /crash "ota"). Returns the
// length written (< len), or 0 if it did not fit.
size_t write_json(char *buf, size_t len);

// GET /ota, POST /ota?mark=valid|invalid (X-NBP-Confirm: yes; only while
// pending).
void register_endpoints(httpd_handle_t srv);

#if NBP_OTA_SELFTEST
namespace selftest {
void run();  // QEMU test build only; never returns
}
#endif

}  // namespace ota_verify
