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

}  // namespace ble_backend
