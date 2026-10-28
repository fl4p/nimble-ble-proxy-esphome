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

// Opaque id of the API client that opened a link (assigned by api_server,
// monotonic, never reused while the device is up). kNoOwner marks a slot
// whose client has gone away and whose link is being torn down.
using OwnerId = uint32_t;
constexpr OwnerId kNoOwner = 0;

// Fired when the connect attempt resolves OR when a connected peer
// later disconnects on its own (then with connected=false). `owner` is
// the client that opened the link, or kNoOwner if it has since left —
// the callee must then not report to anyone.
using ConnectCallback = void (*)(OwnerId owner, uint64_t address,
                                 const ConnectionResult &);

// Fired whenever a slot becomes free OR a previously-free slot is
// claimed. Used to drive BluetoothConnectionsFreeResponse updates.
using FreeChangeCallback = void (*)();

void init();
void register_free_change_cb(FreeChangeCallback cb);

// Asynchronous connect on behalf of API client `owner` (!= kNoOwner).
// Returns false — and does not invoke `cb` — if no slot is free or ANY
// client (including `owner`) already holds a slot for this address. On
// true, `cb` is invoked once with the result, and again later if the
// peer drops.
bool connect(uint64_t address, uint8_t address_type, OwnerId owner,
             ConnectCallback cb);

enum class DisconnectStatus : uint8_t {
  Started,   // teardown issued; cb(owner, addr, {connected:false}) follows
  NotFound,  // no slot for this address
  NotOwner,  // another client holds the link; left untouched
};

// Disconnect (or cancel a still-pending connect to) `address` on behalf
// of `owner`. Only the owning client may tear its link down.
DisconnectStatus disconnect(uint64_t address, OwnerId owner);

// Drop every link `owner` holds (connected or still connecting) and mark
// those slots ownerless so no further state is reported to it. Called
// when that API client goes away while others stay connected.
void release_owner(OwnerId owner);

// How many slots are currently unallocated.
uint8_t free_slots();

// Populate `out` (cap=proxy::MAX_CONNECTIONS) with addresses of slots
// currently in use. Returns the number written.
uint8_t in_use_addresses(uint64_t *out, uint8_t cap);

// Tear down every active connection regardless of owner. Backstop for
// the last API client leaving.
void disconnect_all();

// Internal accessor for gatt_discovery / read / write: returns the
// NimBLEClient* for `address` if it is Connected AND owned by `owner`,
// else nullptr. Type-erased to keep the header NimBLE-free.
void *client_for(uint64_t address, OwnerId owner);

#ifdef CONFIG_NBP_SMP
// SMP passkey used when a peer requests KEYBOARD_ONLY pairing.
// Runtime-mutable via POST /clone?passkey=NNNNNN (merged from the
// previous /passkey endpoint). Default 123456 covers most Victron
// SmartShunts and many ESP32-based peripherals.
void set_passkey(uint32_t pin);
uint32_t get_passkey();
#endif

}  // namespace ble_backend::connection
