// Which API-client traffic the liveness watchdog may take as proof of a
// working uplink. Pure, no IDF dependencies, so it builds on the host
// (host_test/test_stable_session.cpp).
//
// A session that keeps reconnecting is a symptom of a bad uplink, and every
// reconnect handshake is fresh RX. So only RX from a session that has been
// up for at least `min_age_us` counts.

#pragma once

#include <cstddef>
#include <cstdint>

namespace api_server {
namespace stable_session {

struct Snap {
  int64_t start_us;    // esp_timer time the session started; 0 = slot unused
  int64_t last_rx_us;  // last frame received in this session; 0 = none yet
};

// Newest last_rx_us among live sessions at least `min_age_us` old at `now_us`,
// or 0 if there is none.
inline int64_t newest_rx(const Snap *s, size_t n, int64_t now_us, int64_t min_age_us) {
  int64_t best = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i].start_us <= 0) continue;
    if (now_us - s[i].start_us < min_age_us) continue;
    if (s[i].last_rx_us > best) best = s[i].last_rx_us;
  }
  return best;
}

}  // namespace stable_session
}  // namespace api_server
