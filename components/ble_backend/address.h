// BLE address ↔ uint64 conversion utilities.
//
// aioesphomeapi packs a 6-byte BLE address into a uint64 such that
// the MSB of the printed MAC lands in the most-significant non-zero
// byte of the int — MAC AA:BB:CC:DD:EE:FF is 0x0000AABBCCDDEEFF.
//
// NimBLE stores 6 LE bytes (byte[0] = lowest byte of the MAC as
// transmitted = "FF" in the example above). On a little-endian host
// `static_cast<uint64_t>(addr)` does memcpy onto a uint64, which
// places byte[5]=0xAA in bits 40-47 and byte[0]=0xFF in bits 0-7
// — i.e. it ALREADY produces the layout aioesphomeapi expects. No
// swap is needed at the NimBLE → aioesphomeapi boundary.
//
// Address type values match the BLE spec: 0=public, 1=random,
// 2=public_id (RPA resolved to public), 3=random_id, 4=anonymous.

#pragma once

#include <array>
#include <cstdint>

namespace ble_backend::address {

using AddrBytes = std::array<uint8_t, 6>;

// MSB-first byte ordering: bytes[0] is most-significant.
uint64_t bytes_to_uint64(const AddrBytes &bytes);

// Inverse of bytes_to_uint64. Uses low 48 bits of value.
AddrBytes uint64_to_bytes(uint64_t value);

// NimBLE stores addresses little-endian on the wire. Use these at the
// NimBLE boundary when constructing NimBLEAddress from an
// aioesphomeapi-style uint64 (or pulling raw bytes out of one).
uint64_t nimble_le_to_uint64(const uint8_t le_bytes[6]);
void uint64_to_nimble_le(uint64_t value, uint8_t le_bytes[6]);

}  // namespace ble_backend::address
