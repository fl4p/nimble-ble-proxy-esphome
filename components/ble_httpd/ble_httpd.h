// HTTP-style request/response transport over a single NimBLE peripheral
// GATT service. Lets a Web Bluetooth page act as the dashboard frontend
// when NBP_WIFI is off.
//
// Wire format (per fragment, on both REQUEST and RESPONSE chars):
//   byte 0   flags  bit0 = FIN (last fragment of message)
//                   bit1 = ERR (response payload is an error string)
//   byte 1   reqId  echoed by the server so the client can multiplex
//   bytes 2..N      payload
//
// Stub scope: single connection, one request at a time, request must
// fit in a single MTU. Multi-fragment reassembly + multiple in-flight
// requests come later.

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_BLE_HTTPD

namespace ble_httpd {

// Create the GATT service objects (REQUEST / RESPONSE / INFO chars)
// and install the request callback. Does NOT yet call server->start()
// or begin advertising — those are deferred so cloned services added
// later by ble_clone end up in the same registration round.
//
// Must be called AFTER ble_backend::start() so NimBLEDevice::init()
// has run. Idempotent.
void start();

// Finalize: register every service currently in NimBLEServer's vector
// (ble_httpd's + any cloned ones) into the NimBLE host GATT DB, then
// begin advertising the dashboard UUID. Idempotent — calling after
// the first successful activation is a no-op so clone supervisor
// signaling "built" and main's fallback timer can both call it.
void activate();

}  // namespace ble_httpd

#endif  // CONFIG_NBP_BLE_HTTPD
