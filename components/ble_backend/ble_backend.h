// NimBLE-backed BLE proxy backend. Owns the NimBLE host init, the
// scanner, and the pool of concurrent GATT client connections. The
// api_server component routes requests in; this side emits async
// responses via api_server::send_async.

#pragma once

#include <cstdint>

namespace ble_backend {

// Initialize NimBLE host and start the scanner. Must be called after
// wifi is up (NimBLE doesn't depend on wifi but we serialize startup
// to keep logs sane). Idempotent.
void start();

// Called by api_server when the API client goes away. Stops adv
// forwarding and disconnects all GATT links.
void on_api_client_disconnect();

// Diagnostic accessors: NOTIFY_RX events seen at the NimBLE host level
// (independent of NimBLE-Arduino's dispatcher). Useful for telling
// "peer never sent notifies" apart from "NimBLE-Arduino dropped them
// at handle lookup".
uint32_t notify_rx_total();
uint16_t last_notify_handle();

// Configured peripheral advertising interval, in 0.625 ms units. 0
// means "use NimBLE host default" (30..60 ms for connectable undirected
// adv when CONFIG_BT_NIMBLE_HIGH_DUTY_ADV_ITVL is off). Consumers that
// call NimBLEAdvertising::reset() must re-apply after reset; the
// non-reset path picks the value up via the singleton.
uint16_t adv_interval_units();

// Set the desired advertising interval (in ms). 0 reverts to host
// default; 20..10240 ms is the BLE-spec range. Updates the singleton
// NimBLEAdvertising params immediately and hot-restarts adv if it's
// currently active so a slider change takes effect within one cycle.
// NVS persistence is the caller's responsibility (api_server::stats).
void set_adv_interval_ms(uint16_t ms);

// Master enable for the device's own peripheral advertising. Default
// true. When set false, any in-flight advertising is stopped at once and
// every adv start path (clone mirror, ble_httpd, the reconnect-resume
// callbacks) becomes a no-op, so the device stops broadcasting and being
// connectable as a peripheral. The central-role scanner and raw-advert
// forwarding to HA are unaffected — this only gates the broadcaster role.
// Re-enabling resumes advertising with the last-configured payload
// (NimBLE retains it across stop()). Driven by the /advitvl config surface
// (ms=-1 = off); NVS persistence + boot-apply live in api_server::stats
// (key "adv_itvl", 0xFFFF sentinel = off).
bool advertising_enabled();
void set_advertising_enabled(bool on);

// Auto-suspend gate for advertising, owned by the power supervisor and
// orthogonal to the user master switch above. Advertising actually runs
// only when (user-enabled AND NOT auto-suspended); advertising_enabled()
// reports that combined state so every adv start path honors both. The
// supervisor sets this true to silence the broadcaster role when WiFi is
// up and no central/clone needs it, and false to restore it. NVS-free —
// it tracks live conditions, not user intent.
void set_advertising_auto_suspend(bool suspend);

// Full runtime BLE shutdown (the dashboard BLE-TX "off" choice). Tears down
// the entire NimBLE stack — host + controller — via NimBLEDevice::deinit,
// reclaiming the stack's heap and ending all BLE airtime. This is heavier and
// more destructive than the auto-off radio quiesce: the ble_httpd recovery
// dashboard and any clone mirror are torn down too, and (like WiFi-off) it is
// runtime-only — there is no live re-enable, a reboot brings BLE back at its
// configured settings. Idempotent. powered_off() reports the latched state
// (used by api_server to surface "off" and to stay clear of the now-freed
// scanner/connection singletons after shutdown).
bool power_off();
bool powered_off();

// Reversible radio quiesce for an OTA download (vs the destructive power_off):
// pauses the central scan and suspends advertising so coex can't starve the
// firmware upload, holding that state against the auto-off supervisor. Restore
// with resume_after_ota() — called only on OTA failure, since a successful OTA
// reboots. Wired into ota::set_quiesce_hooks by main.
void quiesce_for_ota();
void resume_after_ota();

// Optional radio auto-quiesce supervisor (CONFIG_NBP_BLE_AUTO_OFF). Watches
// who needs the radio and powers down the two BLE roles independently:
//   - central scan: paused when no API client + no GATT links + no clone
//   - peripheral adv: suspended when WiFi up + no central + no clone
// Both come back instantly when needed; idle→off waits the configured
// grace period. The predicates that touch other components (api_server,
// wifi_sta, ble_clone) are injected by main to avoid link-time cycles,
// mirroring publish::install.
namespace power {

struct Hooks {
  bool (*api_client_connected)();  // any ESPHome API client connected
  bool (*wifi_connected)();        // STA associated + has IP (false ⇒ keep adv up)
  bool (*clone_active)();          // BLE clone enabled/running
};

// Spawn the supervisor task. Idempotent; second call is a no-op. Null
// hook pointers are treated as "role needed" (conservative — never
// quiesce on missing information).
void init(const Hooks &hooks);

}  // namespace power

}  // namespace ble_backend
