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
// Runtime-mutable via POST /bond?passkey=NNNNNN (or the /clone form,
// which funnels into the same setter). Default 123456 covers most
// Victron SmartShunts and many ESP32-based peripherals.
void set_passkey(uint32_t pin);
uint32_t get_passkey();

// ---- bonding (SMP pairing) ----
//
// Two entry points, because they answer two different needs:
//
//   * pair() is the on-demand path driven by HA/bleak-esphome's
//     BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR. It runs on an already
//     established link.
//   * the auto-bond list is for peers that refuse *service discovery*
//     on an unauthenticated link (Felicity/SolarB packs are the
//     motivating case). Those drop the connection before HA ever gets
//     the chance to ask for a pairing, so for a listed address we bond
//     right after the GAP connect completes and only report the
//     connection to HA once SMP has resolved.

struct PairResult {
  bool paired;    // link encrypted and (if the peer agreed) bonded
  int32_t error;  // 0 on success, else a NimBLE host code / sentinel
};

using PairCallback = void (*)(uint64_t address, const PairResult &);

// Start SMP on an established link. Returns false when the address is
// not connected or a bonding attempt is already in flight for it — the
// caller then answers its requester itself. On true, `cb` fires exactly
// once; for an already-encrypted link that happens synchronously,
// otherwise from the bonding worker task.
bool pair(uint64_t address, PairCallback cb);

// Delete the stored bond. Returns false when no bond was held.
bool unpair(uint64_t address);

// Copy stored bond addresses into `out`. Returns the number written.
uint8_t bonded_addresses(uint64_t *out, uint8_t cap);

// Auto-bond list. Capped at the NimBLE bond-store size so the list can
// never describe more peers than the device can actually remember.
inline constexpr uint8_t AUTO_BOND_MAX = 4;
void set_auto_bond_list(const uint64_t *addrs, uint8_t n);
uint8_t get_auto_bond_list(uint64_t *out, uint8_t cap);
#endif

}  // namespace ble_backend::connection
