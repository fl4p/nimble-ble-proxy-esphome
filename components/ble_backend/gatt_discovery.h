// GATT service/characteristic/descriptor discovery. Runs synchronously
// in the caller's task (NimBLE walks the peer's attribute table over
// air; this blocks for ~hundreds of ms for typical peripherals).
//
// V1 emits ONE BluetoothGATTGetServicesResponse per request — fine for
// the ~8 services / 12 chars / 6 descriptors limits set in
// api_subset.options. Real chunking against proxy::GATT_DISCOVERY_CHUNK_BYTES
// is a v0.2 concern when proxying complex peripherals.

#pragma once

#include <cstddef>
#include <cstdint>

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
// connected. Emits one services response + one done response (or one
// error response) to `owner` only (publish::send_to).
// Returns the final status; callers usually log + ignore.
//
// Only the owner can reach a link, and each client's requests run
// serially on its own api_client task, so two discoveries never walk the
// same NimBLEClient concurrently (their service lists cannot interleave).
Status run(uint64_t address, uint32_t owner);

#if NBP_OOM_SELFTEST
// QEMU-only: run() for this address skips the BLE link and discovery and
// goes straight to the encode-ctx allocation and the reply.
inline constexpr uint64_t kOomSelftestAddress = 0x0000DEADBEEF0001ULL;
#endif

}  // namespace ble_backend::gatt_discovery
