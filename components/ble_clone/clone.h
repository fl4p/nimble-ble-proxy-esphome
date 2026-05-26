// BLE clone: top-level lifecycle. Compiled in only when CONFIG_NBP_CLONE=y.
//
// Pipeline (single supervisor task):
//   1. config::load()                target MAC + flags from NVS
//   2. upstream::scan_and_connect()  passive scan until target seen, then
//                                    connect as central (uses one of the
//                                    NimBLE conn slots from ble_backend)
//   3. upstream::discover()          walk services / chars / descriptors
//   4. mirror::build_from(client)    build identical local NimBLE GATT DB
//                                    and register it (one-shot in NimBLE)
//   5. upstream::subscribe_all_notifies()
//                                    subscribe to every NOTIFY/INDICATE
//                                    char so notifies start flowing
//   6. mirror::start_advertising()   begin advertising the cloned image
//   7. supervise: on upstream drop, reconnect+resubscribe; the local
//                 GATT DB stays registered so local centrals see a
//                 stale-but-present device during outages
//
// init() returns immediately; the supervisor runs on its own task.
// register_endpoints() exposes /clone (config) and /clone/status (read-
// only stats) on the OTA httpd, mirroring the SMP / stats pattern.

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_CLONE

#include <cstddef>
#include <cstdint>

namespace ble_clone {

void init();
void register_endpoints(void *httpd_handle);

// Transport-agnostic helpers, shared by clone_get / clone_post and by
// the BLE dispatcher in components/ble_httpd. Same JSON shape as the
// /clone HTTP endpoint.
size_t build_clone_json(char *buf, size_t cap);
// Apply `key=value&…` updates from `query` (same params as the HTTP
// POST). Returns nullptr on success and sets `*reboot_required` to
// true when the target MAC changed after the mirror has already been
// built (NimBLE GATT DB is one-shot). Returns a short error message
// on validation / persistence failures.
const char *handle_clone_set(const char *query, bool *reboot_required);

}  // namespace ble_clone

#endif  // CONFIG_NBP_CLONE
