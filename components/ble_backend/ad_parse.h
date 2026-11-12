// Allocation-free readers for a raw advertising payload (AD structures:
// length, type, value[length-1]).
//
// NimBLEAdvertisedDevice's getName() / getManufacturerData() /
// getServiceData() return std::string by value, so any field longer than
// the 15-byte SSO buffer is a heap allocation through the throwing
// operator new — on the NimBLE host task, for every advert. These read
// the same fields straight out of getPayload() instead.
//
// Header-only and dependency-free so it can be tested on the host.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ble_backend::ad {

inline constexpr uint8_t kIncomplete16 = 0x02;
inline constexpr uint8_t kComplete16 = 0x03;
inline constexpr uint8_t kIncomplete128 = 0x06;
inline constexpr uint8_t kComplete128 = 0x07;
inline constexpr uint8_t kShortName = 0x08;
inline constexpr uint8_t kCompleteName = 0x09;
inline constexpr uint8_t kServiceData16 = 0x16;
inline constexpr uint8_t kManufacturer = 0xFF;

struct Field {
  uint8_t type;
  const uint8_t *value;
  size_t len;
};

// Walk the AD structures. `fn(const Field &)` returns true to stop.
// Stops at a zero length byte (padding) or at a structure that runs past
// the end of the payload.
template <typename Fn>
inline void for_each(const uint8_t *p, size_t n, Fn &&fn) {
  size_t i = 0;
  while (p != nullptr && i + 1 < n) {
    const size_t flen = p[i];
    if (flen == 0 || i + 1 + flen > n) return;
    Field f{p[i + 1], p + i + 2, flen - 1};
    if (fn(f)) return;
    i += 1 + flen;
  }
}

// First field of `type` with a non-empty value. False if absent.
inline bool find(const uint8_t *p, size_t n, uint8_t type, Field *out) {
  bool found = false;
  for_each(p, n, [&](const Field &f) {
    if (f.type != type || f.len == 0) return false;
    *out = f;
    found = true;
    return true;
  });
  return found;
}

// Complete local name, else the shortened one.
inline bool find_name(const uint8_t *p, size_t n, Field *out) {
  return find(p, n, kCompleteName, out) || find(p, n, kShortName, out);
}

// Service data for a 16-bit UUID, without the UUID bytes.
inline bool find_service_data16(const uint8_t *p, size_t n, uint16_t uuid,
                                Field *out) {
  bool found = false;
  for_each(p, n, [&](const Field &f) {
    if (f.type != kServiceData16 || f.len < 2) return false;
    if ((uint16_t(f.value[0]) | (uint16_t(f.value[1]) << 8)) != uuid) {
      return false;
    }
    *out = Field{f.type, f.value + 2, f.len - 2};
    found = true;
    return true;
  });
  return found;
}

}  // namespace ble_backend::ad
