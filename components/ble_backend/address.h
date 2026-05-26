// BLE address ↔ uint64 conversion utilities.
//
// aioesphomeapi packs a 6-byte BLE address into a uint64 in big-endian
// order — i.e. AA:BB:CC:DD:EE:FF becomes 0x0000AABBCCDDEEFF. The high
// 16 bits are always zero. NimBLE's `ble_addr_t::val` and
// `NimBLEAddress::getNative()` store the same bytes in *little*-endian
// order over the air, so a swap is needed at the boundary.
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
// NimBLE boundary.
uint64_t nimble_le_to_uint64(const uint8_t le_bytes[6]);
void uint64_to_nimble_le(uint64_t value, uint8_t le_bytes[6]);

// NimBLEAddress::operator uint64_t() memcpys its 6 internal LE bytes into
// a uint64 on a little-endian host, so byte[0] (LSB of OTA byte stream)
// lands in bits 0-7. aioesphomeapi wants the address packed MSB-first.
// Reverse the low 6 bytes.
uint64_t swap6(uint64_t le_packed);

}  // namespace ble_backend::address
