// GATT service/characteristic/descriptor discovery. Runs synchronously
// in the caller's task (NimBLE walks the peer's attribute table over
// air; this blocks for ~hundreds of ms for typical peripherals).
//
// Emits one BluetoothGATTGetServicesResponse per service (any number of
// services; 12 chars / 6 descriptors each, api_subset.options), then the
// Done response.

#pragma once

#include <cstddef>
#include <cstdint>

class NimBLEClient;

namespace ble_backend::gatt_discovery {

enum class Status : int32_t {
  Ok = 0,
  NotConnected = -1,
  TooManyServices = -2,
  EncodeFailed = -3,
  SendFailed = -4,
  NoMemory = -5,
};

// Run discovery on a connected slot owned by API client `owner` (see
// connection::OwnerId). A link held by another client is treated as not
// connected. Emits one services response per service + one done response (or one
// error response) to `owner` only (publish::send_to).
// Returns the final status; callers usually log + ignore.
//
// Only the owner can reach a link, and each client's requests run
// serially on its own api_client task, so two discoveries never walk the
// same NimBLEClient concurrently (their service lists cannot interleave).
//
// The attribute cache is walked over the air only when it is empty (every
// connect clears it). A later GetServices on the same connection answers
// from the cache; after a discovery that failed part-way it fills only the
// gaps. A full rediscovery would delete the characteristics, and with them
// the notify callbacks HA registered, under the host task: notifications
// were dropped as "unknown handle" during the walk and silently after it.
Status run(uint64_t address, uint32_t owner);

// Attribute-cache helpers shared with the GATT read/write/notify handlers.
// All run on an API connection task (they block on the air), never on the
// NimBLE host task.
//
// ensure_discovered: walk the peer once if the cache is empty; a failure
// part-way marks the link partial. Returns false if that walk failed.
bool ensure_discovered(NimBLEClient *client);
// The last walk of `client` failed part-way, so the cache may lack entries.
bool is_partial(NimBLEClient *client);
// Fill only what is missing (services without characteristics,
// characteristics without descriptors). Deletes nothing.
void fill_gaps(NimBLEClient *client);

#if NBP_OOM_SELFTEST
// QEMU-only: run() for this address skips the BLE link and discovery and
// goes straight to the encode-ctx allocation and the reply.
inline constexpr uint64_t kOomSelftestAddress = 0x0000DEADBEEF0001ULL;
#endif

}  // namespace ble_backend::gatt_discovery
