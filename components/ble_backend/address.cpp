#include "address.h"

namespace ble_backend::address {

uint64_t bytes_to_uint64(const AddrBytes &bytes) {
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) {
    v = (v << 8) | bytes[i];
  }
  return v;
}

AddrBytes uint64_to_bytes(uint64_t value) {
  AddrBytes b{};
  for (int i = 5; i >= 0; --i) {
    b[i] = static_cast<uint8_t>(value & 0xff);
    value >>= 8;
  }
  return b;
}

uint64_t nimble_le_to_uint64(const uint8_t le_bytes[6]) {
  // Wire is little-endian (LSB first); we want MSB-first uint64.
  uint64_t v = 0;
  for (int i = 5; i >= 0; --i) {
    v = (v << 8) | le_bytes[i];
  }
  return v;
}

void uint64_to_nimble_le(uint64_t value, uint8_t le_bytes[6]) {
  for (int i = 0; i < 6; ++i) {
    le_bytes[i] = static_cast<uint8_t>(value & 0xff);
    value >>= 8;
  }
}

}  // namespace ble_backend::address
