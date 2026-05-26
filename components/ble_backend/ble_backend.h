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

}  // namespace ble_backend
