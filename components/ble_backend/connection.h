// Pool of concurrent NimBLE GATT client connections. Each slot owns a
// NimBLEClient instance and tracks its state machine. Slot count is
// proxy::MAX_CONNECTIONS.

#pragma once

#include "sdkconfig.h"

#include <cstdint>

namespace ble_backend::connection {

struct ConnectionResult {
  bool connected;
  uint16_t mtu;
  int32_t error;  // 0 on success; otherwise NimBLE / errno-ish code
};

// Fired when the connect attempt resolves OR when a connected peer
// later disconnects on its own (then with connected=false, error=0).
using ConnectCallback = void (*)(uint64_t address, const ConnectionResult &);

// Fired whenever a slot becomes free OR a previously-free slot is
// claimed. Used to drive BluetoothConnectionsFreeResponse updates.
using FreeChangeCallback = void (*)();

void init();
void register_free_change_cb(FreeChangeCallback cb);

// Synchronous connect (blocks the calling task for up to
// proxy::CONNECT_TIMEOUT_MS). Returns false if no slot is free or the
// address is already connected. On success, `cb` is invoked once with
// the result (success / failure), and again later if the peer drops.
bool connect(uint64_t address, uint8_t address_type, ConnectCallback cb);

// Synchronous disconnect. Fires cb(address, {connected:false}) once the
// disconnect completes (or immediately if the slot is already free).
void disconnect(uint64_t address);

// How many slots are currently unallocated.
uint8_t free_slots();

// Populate `out` (cap=proxy::MAX_CONNECTIONS) with addresses of slots
// currently in use. Returns the number written.
uint8_t in_use_addresses(uint64_t *out, uint8_t cap);

// Tear down every active connection. Called when the API client
// disconnects (HA went away) so we don't hold links open uselessly.
void disconnect_all();

// Internal accessor for gatt_discovery / read / write: returns the
// NimBLEClient* for the given address, or nullptr if not connected.
// Type-erased to keep the header NimBLE-free.
void *client_for(uint64_t address);

#ifdef CONFIG_NBP_SMP
// SMP passkey used when a peer requests KEYBOARD_ONLY pairing.
// Runtime-mutable via POST /clone?passkey=NNNNNN (merged from the
// previous /passkey endpoint). Default 123456 covers most Victron
// SmartShunts and many ESP32-based peripherals.
void set_passkey(uint32_t pin);
uint32_t get_passkey();
#endif

}  // namespace ble_backend::connection
