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
};

// Run discovery on a connected slot. Emits one services response + one
// done response (or one error response) using api_server::send_async.
// Returns the final status; callers usually log + ignore.
Status run(uint64_t address);

}  // namespace ble_backend::gatt_discovery
