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

enum class ConnectStatus : uint8_t {
  Started,           // connect issued; cb fires with the result
  OwnConnected,      // `owner` already holds this link, up; *mtu_out set
  OwnInProgress,     // `owner` already has this connect in flight
  HeldByOther,       // another client holds the address (or owner's link
                     // is being torn down) — refused
  NoSlot,            // all slots in use — refused
};

// Asynchronous connect on behalf of API client `owner` (!= kNoOwner).
// Only Started invokes `cb` (once with the result, again later if the
// peer drops). OwnConnected fills *mtu_out (may be nullptr).
ConnectStatus connect(uint64_t address, uint8_t address_type, OwnerId owner,
                      ConnectCallback cb, uint16_t *mtu_out);

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

// Tear down every slot that has NO owner (its client left) and is not
// already winding down. Backstop for the last API client leaving; slots
// owned by a live client are never touched — one may have registered and
// issued a CONNECT between "count hit 0" and this call.
void disconnect_unowned();

// Internal accessor for gatt_discovery / read / write: returns the
// NimBLEClient* for `address` if it is Connected AND owned by `owner`,
// else nullptr. Type-erased to keep the header NimBLE-free.
void *client_for(uint64_t address, OwnerId owner);

// Owner of the Connected slot backed by `client` (a NimBLEClient*), or
// kNoOwner if none / its client has left. For routing notify data.
OwnerId owner_of_client(const void *client);

#ifdef CONFIG_NBP_SMP
// SMP passkey used when a peer requests KEYBOARD_ONLY pairing.
// Runtime-mutable via POST /bond?passkey=NNNNNN, POST /clone?passkey=NNNNNN,
// or POST /passkey?val=NNNNNN in builds without clone (all funnel into the
// same setter). Default 123456 covers most Victron SmartShunts and many
// ESP32-based peripherals.
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

// `owner` is the API client that asked (the link's owner); the callee
// reports to it alone.
using PairCallback = void (*)(OwnerId owner, uint64_t address,
                              const PairResult &);

// Start SMP on an established link owned by `owner`. Returns false when
// the address is not connected, is held by another client, its connect
// result is still being withheld by an auto-bond, or a bonding attempt
// is already in flight for it — the caller then answers its requester
// itself. On true, `cb` fires exactly once; for an already-encrypted
// link that happens synchronously, otherwise from the bonding worker
// task.
bool pair(uint64_t address, OwnerId owner, PairCallback cb);

// True when the bond store holds keys for this peer, connected or not.
// False after a runtime BLE power-off — there is no host left to ask.
bool has_bond(uint64_t address);

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
