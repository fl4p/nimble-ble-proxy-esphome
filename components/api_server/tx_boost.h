// Self-reverting WiFi TX-power boost: the pure parts (argument parsing and
// the remaining-time readout), so they build on the host
// (host_test/test_tx_boost.cpp). The esp_timer and esp_wifi calls live in
// stats.cpp.
//
// POST /txpower?wifi=<dBm>&for=<s> runs the radio at <dBm> for <s> seconds
// and then falls back on its own to the persisted level. The boost is never
// written to NVS, so a reboot or a client that dies mid-boost cannot leave
// the board at the boosted level. 2026-11-18, .231: the AP dropped it for
// missing ACKs (reason 34) at 11 dBm, and fugu's ota_mqtt.py boosts it only
// for the ~2 min of a fry/flat OTA routed through it.

#pragma once

#include <cstdint>
#include <cstdlib>

namespace api_server {
namespace tx_boost {

// Longest boost one request may ask for. A client that wants longer must
// renew; one that dies is reverted within this bound.
constexpr uint32_t MAX_FOR_S = 900;

// Parse a `for=` value: a plain decimal integer 1..MAX_FOR_S. Returns null on
// success (with *out set) or a static error string.
inline const char *parse_for(const char *s, uint32_t *out) {
  if (s == nullptr || *s == '\0') return "for: 1..900 seconds";
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 1 || v > static_cast<long>(MAX_FOR_S)) {
    return "for: 1..900 seconds";
  }
  *out = static_cast<uint32_t>(v);
  return nullptr;
}

// Seconds until an active boost reverts, rounded up so a live boost never
// reads 0; 0 when no boost is active (until_us == 0) or it has expired.
inline uint32_t remaining_s(int64_t until_us, int64_t now_us) {
  if (until_us <= 0 || until_us <= now_us) return 0;
  return static_cast<uint32_t>((until_us - now_us + 999999) / 1000000);
}

}  // namespace tx_boost
}  // namespace api_server
